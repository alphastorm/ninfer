#pragma once

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace ninfer::runtime {

// Compressed edges refer to a surviving native checkpoint's semantic digest path. The model
// verifies exact token/position/media identity before a returned checkpoint can be used.
template <class Model>
class PrefixIndex {
    struct Node;
public:
    using Handle  = typename Model::CheckpointHandle;
    using Program = typename Model::Program;
    using Base    = typename Model::RequestBasePlan;

    struct InsertReservation {
        std::vector<std::unique_ptr<Node>> nodes;
    };

    // Reserve at the publication boundary, before native commit. At most one branch and
    // one leaf are introduced per point; all existing nodes may receive those points.
    [[nodiscard]] InsertReservation reserve_insert(std::size_t count) {
        reserve_node(root_, count);
        InsertReservation out;
        out.nodes.reserve(2 * count);
        for (std::size_t i = 0; i < 2 * count; ++i) {
            auto node = std::make_unique<Node>();
            node->children.reserve(count + 1);
            node->checkpoints.reserve(count);
            out.nodes.push_back(std::move(node));
        }
        return out;
    }

    void insert(Program& program, Handle handle, std::uint32_t frontier) {
        insert_at(program, root_, handle, frontier, nullptr);
    }

    void insert(Program& program, Handle handle, std::uint32_t frontier,
                InsertReservation& reserved) {
        insert_at(program, root_, handle, frontier, &reserved);
    }

    void erase(Handle handle) { erase_at(root_, handle, true); }

    [[nodiscard]] std::vector<Handle> matches(const Program& program, const Base& base) const {
        std::vector<Handle> result;
        const Node* current = &root_;
        while (current) {
            result.insert(result.end(), current->checkpoints.begin(), current->checkpoints.end());
            const Node* next = nullptr;
            for (const auto& child : current->children) {
                if (child->frontier > base.summary().prompt_tokens) { continue; }
                const auto key = base.prefix_shortlist_key(child->frontier);
                if (key && *key == program.checkpoint_key(child->representative, child->frontier)) {
                    next = child.get();
                    break;
                }
            }
            current = next;
        }
        std::reverse(result.begin(), result.end());
        return result;
    }

    void clear() noexcept { root_ = Node{}; }

private:
    struct Node {
        std::uint32_t frontier = 0;
        Handle representative{};
        std::vector<Handle> checkpoints;
        std::vector<std::unique_ptr<Node>> children;
    };

    static void reserve_node(Node& node, std::size_t count) {
        node.children.reserve(node.children.size() + count);
        node.checkpoints.reserve(node.checkpoints.size() + count);
        for (auto& child : node.children) { reserve_node(*child, count); }
    }

    static std::unique_ptr<Node> make_node(InsertReservation* reserved) {
        if (!reserved) { return std::make_unique<Node>(); }
        auto result = std::move(reserved->nodes.back());
        reserved->nodes.pop_back();
        return result;
    }

    static void insert_at(Program& program, Node& node, Handle handle, std::uint32_t frontier,
                          InsertReservation* reserved) {
        if (node.frontier == frontier) {
            if (std::find(node.checkpoints.begin(), node.checkpoints.end(), handle) ==
                node.checkpoints.end()) {
                node.checkpoints.push_back(handle);
            }
            node.representative = handle;
            return;
        }
        for (auto& child : node.children) {
            if (program.checkpoint_key(child->representative, node.frontier + 1U) !=
                program.checkpoint_key(handle, node.frontier + 1U)) {
                continue;
            }
            std::uint32_t low = node.frontier + 1U, high = std::min(frontier, child->frontier);
            while (low < high) {
                const auto middle = low + (high - low + 1U) / 2U;
                if (program.checkpoint_key(child->representative, middle) ==
                    program.checkpoint_key(handle, middle)) {
                    low = middle;
                } else {
                    high = middle - 1U;
                }
            }
            if (low < child->frontier) {
                auto branch            = make_node(reserved);
                branch->frontier       = low;
                branch->representative = child->representative;
                branch->children.push_back(std::move(child));
                child = std::move(branch);
            }
            insert_at(program, *child, handle, frontier, reserved);
            return;
        }
        auto child            = make_node(reserved);
        child->frontier       = frontier;
        child->representative = handle;
        child->checkpoints.push_back(handle);
        node.children.push_back(std::move(child));
    }

    static bool erase_at(Node& node, Handle handle, bool root = false) {
        std::erase(node.checkpoints, handle);
        for (auto it = node.children.begin(); it != node.children.end();) {
            if (erase_at(**it, handle)) {
                it = node.children.erase(it);
            } else {
                ++it;
            }
        }
        if (root) { return false; }
        if (node.checkpoints.empty() && node.children.empty()) { return true; }
        if (node.checkpoints.empty() && node.children.size() == 1) {
            auto child = std::move(node.children.front());
            node       = std::move(*child);
        }
        node.representative = !node.checkpoints.empty() ? node.checkpoints.front()
                                                        : node.children.front()->representative;
        return false;
    }

    Node root_;
};
} // namespace ninfer::runtime
