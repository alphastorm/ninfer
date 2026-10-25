#include "models/common/context/state_store.h"

#include <limits>
#include <stdexcept>

namespace ninfer::models::context {

StateImageStore::ReadLease::~ReadLease() { reset(); }

StateImageStore::ReadLease::ReadLease(ReadLease&& other) noexcept
    : store_(std::exchange(other.store_, nullptr)), image_(other.image_), epoch_(other.epoch_) {}

StateImageStore::ReadLease& StateImageStore::ReadLease::operator=(ReadLease&& other) noexcept {
    if (this != &other) {
        reset();
        store_ = std::exchange(other.store_, nullptr);
        image_ = other.image_;
        epoch_ = other.epoch_;
    }
    return *this;
}

void StateImageStore::ReadLease::reset() noexcept {
    if (!store_) return;
    auto* store = std::exchange(store_, nullptr);
    auto& image = store->require(image_);
    if (!image.source_pins || image.content_epoch != epoch_) std::terminate();
    --image.source_pins;
    store->retire_released_owner(image_);
}

std::int32_t StateImageStore::ReadLease::device_slot() const {
    if (!store_ || store_->content_epoch(image_) != epoch_)
        throw std::logic_error("StateImage reader has no current content");
    return store_->physical_slot(image_);
}

ConstMemorySpan StateImageStore::ReadLease::host_view() const {
    if (!store_ || store_->content_epoch(image_) != epoch_)
        throw std::logic_error("StateImage reader has no current content");
    const auto& image = store_->require(image_);
    if (!image.host_slot) throw std::logic_error("StateImage reader has no Host replica");
    return image.host_slot->span().subspan(0, store_->device_->image_bytes());
}

StateImageStore::ReadLease StateImageStore::read(Handle handle) {
    auto& object = require(handle);
    if (object.role != StateImageRole::CheckpointImmutable ||
        object.source_pins == std::numeric_limits<std::uint32_t>::max())
        throw std::logic_error("StateImage is not readable as an immutable replica");
    ++object.source_pins;
    return ReadLease(*this, handle, object.content_epoch);
}

StateImageStore::Destination::Destination(StateImageStore& store, Handle image)
    : store_(&store), image_(image) {
    if (store.role(image) != StateImageRole::ReservedDestination)
        throw std::logic_error("StateImage destination is not reserved");
}

StateImageStore::Destination::~Destination() {
    if (!store_) return;
    if (write_source_.valid()) store_->abort_write(write_source_, image_);
    if (!store_->release(image_)) std::terminate();
}

StateImageStore::Destination::Destination(Destination&& other) noexcept
    : store_(std::exchange(other.store_, nullptr)), image_(other.image_),
      write_source_(other.write_source_) {}

StateImageSelectors StateImageStore::Destination::begin_write(Handle source) {
    const auto selectors = store_->begin_write(source, image_);
    write_source_        = source;
    return selectors;
}

StateImageStore::Handle StateImageStore::Destination::release() noexcept {
    store_ = nullptr;
    return std::exchange(image_, Handle{});
}

StateImageStore::Transfer::Transfer(StateImageStore& owner, std::uint64_t id,
                                    StateTransferDirection direction, ReadLease source,
                                    Handle destination) noexcept
    : owner_(&owner), id_(id), direction_(direction), source_(std::move(source)),
      destination_(destination) {}

StateImageStore::Transfer::~Transfer() {
    if (owner_) owner_->abort_transfer(*this);
}

StateImageStore::Transfer::Transfer(Transfer&& other) noexcept
    : owner_(std::exchange(other.owner_, nullptr)), id_(std::exchange(other.id_, 0)),
      direction_(other.direction_), source_(std::move(other.source_)),
      destination_(other.destination_),
      device_slot_(std::exchange(other.device_slot_, std::nullopt)),
      host_slot_(std::move(other.host_slot_)) {}

TransferWork StateImageStore::Transfer::work() const {
    if (!owner_) throw std::logic_error("StateImage copy already consumed");
    owner_->validate_transfer(*this);
    return owner_->device_->layout().transfer_work();
}

void StateImageStore::Transfer::enqueue(CudaExecutionView execution) const {
    if (!owner_) throw std::logic_error("StateImage copy already consumed");
    owner_->validate_transfer(*this);
    if (direction_ == StateTransferDirection::DeviceToHost) {
        owner_->device_->copy_to_host(source_.device_slot(), host_slot_->span(), execution);
    } else {
        const auto& source = owner_->require(source_.image());
        owner_->device_->copy_from_host(source.host_slot->span(), *device_slot_, execution);
    }
}

void StateImageStore::Transfer::publish(bool keep_source_replica) {
    if (!owner_) throw std::logic_error("StateImage copy already consumed");
    owner_->publish_transfer(*this, keep_source_replica);
}

StateImageStore::StateImageStore(StateImageStorage& device, HostContextArena* host,
                                 std::uint32_t logical_capacity)
    : device_(&device), host_(host), objects_(logical_capacity), free_objects_(logical_capacity),
      free_device_slots_(static_cast<std::size_t>(device.slot_count())),
      free_object_count_(logical_capacity),
      free_device_count_(static_cast<std::uint32_t>(device.slot_count())) {
    if (logical_capacity == 0 || device.slot_count() <= 0 ||
        logical_capacity < static_cast<std::uint32_t>(device.slot_count()) ||
        (host != nullptr && logical_capacity < static_cast<std::uint32_t>(device.slot_count()) +
                                                   host->capacity_bytes() / device.image_bytes())) {
        throw std::invalid_argument("StateImageStore capacity is inconsistent");
    }
    for (std::uint32_t index = 0; index < logical_capacity; ++index) {
        free_objects_[index] = logical_capacity - 1U - index;
    }
    for (std::uint32_t index = 0; index < free_device_slots_.size(); ++index) {
        free_device_slots_[index] =
            static_cast<std::int32_t>(free_device_slots_.size() - 1U - index);
    }
}

std::uint32_t StateImageStore::capacity() const noexcept {
    return static_cast<std::uint32_t>(objects_.size());
}

std::uint32_t StateImageStore::occupied() const noexcept { return capacity() - free_object_count_; }

std::uint32_t StateImageStore::device_occupied() const noexcept {
    return static_cast<std::uint32_t>(free_device_slots_.size()) - free_device_count_;
}

std::uint32_t StateImageStore::device_capacity() const noexcept {
    return static_cast<std::uint32_t>(free_device_slots_.size());
}

std::uint32_t StateImageStore::host_occupied() const noexcept {
    std::uint32_t count = pending_host_images_;
    for (const auto& object : objects_) { count += object.host_slot.has_value(); }
    return count;
}

std::optional<StateImageStore::Handle> StateImageStore::reserve_destination() noexcept {
    return allocate(StateImageRole::ReservedDestination, true);
}

std::optional<StateImageStore::Handle> StateImageStore::reserve_logical_destination() noexcept {
    return allocate(StateImageRole::ReservedDestination, false);
}

std::optional<StateImageStore::Handle> StateImageStore::reserve_host_destination() noexcept {
    if (!host_ || !free_object_count_) return std::nullopt;
    auto allocation = host_->allocate(device_->image_bytes());
    if (!allocation) return std::nullopt;
    auto image = allocate(StateImageRole::ReservedDestination, false);
    if (!image) return std::nullopt;
    objects_[image->index_].host_slot.emplace(std::move(*allocation));
    return image;
}

MemorySpan StateImageStore::writable_host_view(Handle handle) {
    auto& image = require(handle);
    if (image.role != StateImageRole::ReservedDestination || !image.host_slot ||
        image.device_slot || image.source_pins || image.destination_pinned ||
        has_pending_replica(image))
        throw std::logic_error("Host StateImage initialization has no exclusive reservation");
    return image.host_slot->span().subspan(0, device_->image_bytes());
}

void StateImageStore::complete_host_initialization(Handle handle) {
    (void)writable_host_view(handle);
    auto& image = require(handle);
    image.content_epoch = next_epoch();
    image.host_slot->publish();
    image.role = StateImageRole::CheckpointImmutable;
}

void StateImageStore::complete_initialization(Handle handle) {
    Object& object = require(handle);
    if (object.role != StateImageRole::ReservedDestination || !object.device_slot ||
        object.source_pins != 0 || object.destination_pinned || has_pending_replica(object)) {
        throw std::logic_error("StateImage initialization has no exclusive reservation");
    }
    if (object.content_epoch == 0) { object.content_epoch = next_epoch(); }
    object.role = StateImageRole::ActiveMutable;
}

bool StateImageStore::valid(Handle handle) const noexcept {
    return handle.owner_ == this && handle.index_ < objects_.size() &&
           objects_[handle.index_].role != StateImageRole::Free &&
           objects_[handle.index_].generation == handle.generation_;
}

std::uint32_t StateImageStore::descriptor_index(Handle handle) const {
    (void)require(handle);
    return handle.index_;
}

StateImageRole StateImageStore::role(Handle handle) const { return require(handle).role; }

StateReplicaResidency StateImageStore::residency(Handle handle) const {
    const Object& object = require(handle);
    if (object.device_slot && object.host_slot) { return StateReplicaResidency::Both; }
    if (object.device_slot) { return StateReplicaResidency::DeviceOnly; }
    if (object.host_slot) { return StateReplicaResidency::HostOnly; }
    return StateReplicaResidency::None;
}

bool StateImageStore::device_resident(Handle handle) const {
    return require(handle).device_slot.has_value();
}

bool StateImageStore::host_resident(Handle handle) const {
    return require(handle).host_slot.has_value();
}

std::uint64_t StateImageStore::content_epoch(Handle handle) const {
    return require(handle).content_epoch;
}

std::uint32_t StateImageStore::source_pins(Handle handle) const {
    return require(handle).source_pins;
}

std::uint32_t StateImageStore::checkpoint_references(Handle handle) const {
    return require(handle).checkpoint_references;
}

void StateImageStore::retain_checkpoint_reference(Handle handle) {
    Object& object = require(handle);
    if (object.role != StateImageRole::CheckpointImmutable ||
        object.checkpoint_references == std::numeric_limits<std::uint32_t>::max()) {
        throw std::logic_error("StateImage checkpoint reference is not retainable");
    }
    ++object.checkpoint_references;
    object.release_pending = false;
}

bool StateImageStore::can_release_checkpoint_owner(Handle handle) const noexcept {
    if (!valid(handle)) { return false; }
    const Object& object = objects_[handle.index_];
    return object.role == StateImageRole::CheckpointImmutable && object.checkpoint_references != 0;
}

bool StateImageStore::release_checkpoint_owner(Handle handle) noexcept {
    if (!can_release_checkpoint_owner(handle)) { return false; }
    Object& object = objects_[handle.index_];
    if (--object.checkpoint_references == 0) {
        object.release_pending = true;
        retire_released_owner(handle);
    }
    return true;
}

std::int32_t StateImageStore::physical_slot(Handle handle) const {
    const Object& object = require(handle);
    if (!object.device_slot) { throw std::logic_error("StateImage has no Device storage"); }
    return *object.device_slot;
}

void StateImageStore::make_mutable(Handle handle) {
    Object& object = require(handle);
    if (object.role != StateImageRole::CheckpointImmutable || !object.device_slot ||
        object.checkpoint_references != 0 || object.source_pins != 0 || object.destination_pinned ||
        has_pending_replica(object)) {
        throw std::logic_error("StateImage checkpoint is not movable");
    }
    object.host_slot.reset();
    object.role = StateImageRole::ActiveMutable;
}

void StateImageStore::consume_checkpoint(Handle handle) {
    Object& object = require(handle);
    if (object.role != StateImageRole::CheckpointImmutable || !object.device_slot ||
        object.checkpoint_references != 1 || object.source_pins != 0 || object.destination_pinned ||
        has_pending_replica(object)) {
        throw std::logic_error("StateImage checkpoint has other owners or readers");
    }
    object.checkpoint_references = 0;
    object.release_pending       = false;
    object.host_slot.reset();
    object.role = StateImageRole::ActiveMutable;
}

bool StateImageStore::drop_device_replica(Handle handle) noexcept {
    if (!valid(handle)) { return false; }
    Object& object = objects_[handle.index_];
    if (object.role != StateImageRole::CheckpointImmutable || !object.device_slot ||
        !object.host_slot || object.source_pins != 0 || object.destination_pinned ||
        has_pending_replica(object)) {
        return false;
    }
    return_device_slot(*object.device_slot);
    object.device_slot.reset();
    return true;
}

bool StateImageStore::drop_host_replica(Handle handle) noexcept {
    if (!valid(handle)) { return false; }
    Object& object = objects_[handle.index_];
    if (host_ == nullptr || object.role != StateImageRole::CheckpointImmutable ||
        !object.device_slot || !object.host_slot || object.source_pins != 0 ||
        object.destination_pinned || has_pending_replica(object)) {
        return false;
    }
    object.host_slot.reset();
    return true;
}

void StateImageStore::split_device_replica_identity(Handle source, Handle destination) {
    Object& source_object      = require(source);
    Object& destination_object = require(destination);
    if (source == destination || source_object.role != StateImageRole::CheckpointImmutable ||
        !source_object.device_slot || !source_object.host_slot || source_object.source_pins != 0 ||
        source_object.destination_pinned || has_pending_replica(source_object) ||
        destination_object.role != StateImageRole::ReservedDestination ||
        destination_object.device_slot || destination_object.host_slot ||
        has_pending_replica(destination_object) || destination_object.source_pins != 0 ||
        destination_object.destination_pinned) {
        throw std::logic_error("StateImage replica identity split is invalid");
    }
    destination_object.device_slot   = source_object.device_slot;
    destination_object.content_epoch = source_object.content_epoch;
    source_object.device_slot.reset();
}

void StateImageStore::freeze(Handle handle) {
    Object& object = require(handle);
    if (object.role != StateImageRole::ActiveMutable || !object.device_slot ||
        object.source_pins != 0 || object.destination_pinned || has_pending_replica(object)) {
        throw std::logic_error("StateImage active image is not freezable");
    }
    object.role = StateImageRole::CheckpointImmutable;
}

StateImageSelectors StateImageStore::begin_write(Handle source, Handle destination) {
    Object& source_object      = require(source);
    Object& destination_object = require(destination);
    if (source == destination || source_object.role != StateImageRole::CheckpointImmutable ||
        !source_object.device_slot ||
        source_object.source_pins == std::numeric_limits<std::uint32_t>::max() ||
        destination_object.role != StateImageRole::ReservedDestination ||
        !destination_object.device_slot || destination_object.source_pins != 0 ||
        destination_object.destination_pinned || has_pending_replica(destination_object)) {
        throw std::logic_error("StateImage fork binding is invalid");
    }
    ++source_object.source_pins;
    destination_object.destination_pinned = true;
    destination_object.role               = StateImageRole::Writing;
    destination_object.write_source       = source;
    destination_object.content_epoch      = source_object.content_epoch;
    return {.source = *source_object.device_slot, .destination = *destination_object.device_slot};
}

void StateImageStore::complete_write(Handle source, Handle destination) {
    if (source == destination) {
        (void)selectors(source, destination);
        require(destination).content_epoch = next_epoch();
        return;
    }
    Object& source_object      = require(source);
    Object& destination_object = require(destination);
    if (source_object.role != StateImageRole::CheckpointImmutable ||
        source_object.source_pins == 0 || destination_object.role != StateImageRole::Writing ||
        !destination_object.device_slot || !destination_object.destination_pinned ||
        destination_object.write_source != source) {
        throw std::logic_error("StateImage fork commit is invalid");
    }
    --source_object.source_pins;
    destination_object.destination_pinned = false;
    destination_object.write_source       = {};
    destination_object.role               = StateImageRole::ActiveMutable;
    destination_object.content_epoch      = next_epoch();
    retire_released_owner(source);
}

void StateImageStore::abort_write(Handle source, Handle destination) {
    if (!can_abort_write(source, destination)) {
        throw std::logic_error("StateImage fork abort is invalid");
    }
    Object& source_object      = require(source);
    Object& destination_object = require(destination);
    --source_object.source_pins;
    destination_object.destination_pinned = false;
    destination_object.write_source       = {};
    destination_object.role               = StateImageRole::ReservedDestination;
    destination_object.content_epoch      = 0;
    retire_released_owner(source);
}

bool StateImageStore::can_abort_write(Handle source, Handle destination) const noexcept {
    if (!valid(source) || !valid(destination)) { return false; }
    const Object& source_object      = objects_[source.index_];
    const Object& destination_object = objects_[destination.index_];
    return source != destination && source_object.role == StateImageRole::CheckpointImmutable &&
           source_object.source_pins != 0 && destination_object.role == StateImageRole::Writing &&
           destination_object.device_slot.has_value() && destination_object.destination_pinned &&
           destination_object.write_source == source;
}

StateImageSelectors StateImageStore::selectors(Handle source, Handle destination) const {
    const Object& source_object      = require(source);
    const Object& destination_object = require(destination);
    const bool inplace               = source == destination;
    if (!source_object.device_slot || !destination_object.device_slot ||
        (inplace && (source_object.role != StateImageRole::ActiveMutable ||
                     source_object.source_pins != 0 || source_object.destination_pinned)) ||
        (!inplace &&
         (source_object.role != StateImageRole::CheckpointImmutable ||
          source_object.source_pins == 0 || destination_object.role != StateImageRole::Writing ||
          !destination_object.destination_pinned || destination_object.write_source != source))) {
        throw std::logic_error("StateImage execution binding is invalid");
    }
    return {.source = *source_object.device_slot, .destination = *destination_object.device_slot};
}

std::optional<StateImageStore::Transfer>
StateImageStore::reserve_device_to_host(Handle source, HostContextAllocation reserved) {
    Object& object = require(source);
    if (host_ == nullptr || object.role != StateImageRole::CheckpointImmutable ||
        !object.device_slot || object.host_slot || has_pending_replica(object) ||
        object.source_pins == std::numeric_limits<std::uint32_t>::max()) {
        return std::nullopt;
    }
    if (!reserved.valid()) {
        auto target = host_->allocate(device_->image_bytes());
        if (!target) { return std::nullopt; }
        reserved = std::move(*target);
    }
    if (!reserved.reserved() || reserved.bytes() != device_->image_bytes()) {
        throw std::invalid_argument("StateImage Host reservation has incompatible geometry");
    }
    const auto id = next_transfer();
    Transfer transfer(*this, id, StateTransferDirection::DeviceToHost, read(source), source);
    transfer.host_slot_.emplace(std::move(reserved));
    ++pending_host_images_;
    object.transfer_id        = id;
    object.destination_pinned = true;
    return transfer;
}

std::optional<StateImageStore::Transfer> StateImageStore::reserve_host_to_device(Handle source) {
    return reserve_host_to_device(source, source);
}

std::optional<StateImageStore::Transfer>
StateImageStore::reserve_host_to_device(Handle source, Handle destination) {
    auto& from = require(source);
    auto& to   = require(destination);
    if (!from.host_slot || from.role != StateImageRole::CheckpointImmutable ||
        from.source_pins == std::numeric_limits<std::uint32_t>::max() || to.device_slot ||
        has_pending_replica(to) || to.destination_pinned ||
        (source != destination &&
         (to.role != StateImageRole::ReservedDestination || to.source_pins || to.host_slot)))
        return std::nullopt;
    const auto slot = take_device_slot();
    if (!slot) return std::nullopt;
    const auto id = next_transfer();
    Transfer transfer(*this, id, StateTransferDirection::HostToDevice, read(source), destination);
    transfer.device_slot_ = *slot;
    to.transfer_id        = id;
    to.destination_pinned = true;
    to.content_epoch      = from.content_epoch;
    return transfer;
}

void StateImageStore::publish_transfer(Transfer& transfer, bool keep_source_replica) {
    validate_transfer(transfer);
    const auto source_handle = transfer.source_.image();
    auto& source             = require(source_handle);
    auto& destination        = require(transfer.destination_);
    if (source.content_epoch != transfer.source_.epoch())
        throw std::logic_error("StateImage source changed during migration");
    if (!keep_source_replica && source_handle == transfer.destination_ && source.source_pins != 1)
        throw std::logic_error("StateImage source replica still has readers");
    if (transfer.direction_ == StateTransferDirection::DeviceToHost) {
        transfer.host_slot_->publish();
        source.host_slot.emplace(std::move(*transfer.host_slot_));
        transfer.host_slot_.reset();
        --pending_host_images_;
        if (!keep_source_replica) {
            return_device_slot(*source.device_slot);
            source.device_slot.reset();
        }
    } else {
        destination.device_slot = std::exchange(transfer.device_slot_, std::nullopt);
        if (!keep_source_replica && source_handle == transfer.destination_)
            source.host_slot.reset();
    }
    destination.destination_pinned = false;
    destination.transfer_id        = 0;
    consume(transfer);
    transfer.source_.reset();
}

void StateImageStore::abort_transfer(Transfer& transfer) noexcept {
    if (!valid_transfer(transfer)) std::terminate();
    auto& destination = objects_[transfer.destination_.index_];
    if (transfer.host_slot_) {
        transfer.host_slot_.reset();
        --pending_host_images_;
    }
    if (transfer.device_slot_) {
        return_device_slot(*transfer.device_slot_);
        transfer.device_slot_.reset();
    }
    destination.destination_pinned = false;
    destination.transfer_id        = 0;
    if (transfer.source_.image() != transfer.destination_) destination.content_epoch = 0;
    consume(transfer);
    transfer.source_.reset();
}

bool StateImageStore::release(Handle handle) noexcept {
    if (!can_release(handle)) { return false; }
    Object& object = objects_[handle.index_];
    object.host_slot.reset();
    if (object.device_slot) {
        return_device_slot(*object.device_slot);
        object.device_slot.reset();
    }
    object.role          = StateImageRole::Free;
    object.content_epoch = 0;
    if (++object.generation == 0) { ++object.generation; }
    free_objects_[free_object_count_++] = handle.index_;
    return true;
}

bool StateImageStore::can_release(Handle handle) const noexcept {
    if (!valid(handle)) { return false; }
    const Object& object = objects_[handle.index_];
    return object.checkpoint_references == 0 && object.source_pins == 0 &&
           !object.destination_pinned && !has_pending_replica(object);
}

bool StateImageStore::can_release_after_checkpoint_references(
    Handle handle, std::uint32_t released_references) const {
    const Object& object = require(handle);
    if (released_references > object.checkpoint_references) { return false; }
    if (object.checkpoint_references != released_references) { return true; }
    if (object.source_pins != 0 || object.destination_pinned || has_pending_replica(object)) {
        return false;
    }
    return true;
}

void StateImageStore::retire_released_owner(Handle handle) noexcept {
    if (valid(handle) && objects_[handle.index_].release_pending && can_release(handle) &&
        !release(handle)) {
        std::terminate();
    }
}

bool StateImageStore::has_pending_replica(const Object& object) noexcept {
    return object.transfer_id != 0;
}

std::optional<StateImageStore::Handle> StateImageStore::allocate(StateImageRole role,
                                                                 bool with_device) noexcept {
    if (free_object_count_ == 0 || role == StateImageRole::Free ||
        (with_device && free_device_count_ == 0)) {
        return std::nullopt;
    }
    const std::uint32_t index = free_objects_[--free_object_count_];
    Object& object            = objects_[index];
    object                    = Object{.generation = object.generation, .role = role};
    if (with_device) { object.device_slot = free_device_slots_[--free_device_count_]; }
    return Handle(this, index, object.generation);
}

std::optional<std::int32_t> StateImageStore::take_device_slot() noexcept {
    if (free_device_count_ == 0) { return std::nullopt; }
    return free_device_slots_[--free_device_count_];
}

void StateImageStore::return_device_slot(std::int32_t slot) noexcept {
    free_device_slots_[free_device_count_++] = slot;
}

std::uint64_t StateImageStore::next_epoch() noexcept {
    if (++next_content_epoch_ == 0) { ++next_content_epoch_; }
    return next_content_epoch_;
}

std::uint64_t StateImageStore::next_transfer() noexcept {
    if (++next_transfer_id_ == 0) { ++next_transfer_id_; }
    return next_transfer_id_;
}

StateImageStore::Object& StateImageStore::require(Handle handle) {
    if (!valid(handle)) { throw std::invalid_argument("StateImage handle is stale"); }
    return objects_[handle.index_];
}

const StateImageStore::Object& StateImageStore::require(Handle handle) const {
    if (!valid(handle)) { throw std::invalid_argument("StateImage handle is stale"); }
    return objects_[handle.index_];
}

bool StateImageStore::valid_transfer(const Transfer& transfer) const noexcept {
    if (transfer.owner_ != this || transfer.id_ == 0 || !valid(transfer.source_.image()) ||
        !valid(transfer.destination_)) {
        return false;
    }
    const Object& destination = objects_[transfer.destination_.index_];
    return destination.transfer_id == transfer.id_;
}

void StateImageStore::validate_transfer(const Transfer& transfer) const {
    if (!valid_transfer(transfer)) {
        throw std::logic_error("StateImage transfer capability is stale");
    }
}

void StateImageStore::consume(Transfer& transfer) noexcept {
    transfer.owner_ = nullptr;
    transfer.id_    = 0;
}

} // namespace ninfer::models::context
