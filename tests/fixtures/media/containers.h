#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

// Minimal Matroska and QuickTime files around MJPEG frames. Every timing and display field is
// written explicitly, so tests reach demuxer-reported metadata that encoders do not produce:
// unbounded rotation tags, singular display matrices, absent or inconsistent durations and
// extreme frame rates.
namespace container_fixtures {

using Bytes = std::vector<std::uint8_t>;

inline void append(Bytes& out, std::span<const std::uint8_t> bytes) {
    out.insert(out.end(), bytes.begin(), bytes.end());
}

inline void append_text(Bytes& out, std::string_view text) {
    out.insert(out.end(), text.begin(), text.end());
}

inline void append_zeros(Bytes& out, std::size_t count) { out.insert(out.end(), count, 0); }

// Big-endian unsigned field of 1..8 bytes.
inline void append_be(Bytes& out, std::uint64_t value, int width) {
    for (int shift = 8 * (width - 1); shift >= 0; shift -= 8) {
        out.push_back(static_cast<std::uint8_t>(value >> shift));
    }
}

// EBML element with an eight-byte size field; IDs include their length-marker bits.
inline Bytes ebml(std::uint32_t id, std::span<const std::uint8_t> payload) {
    Bytes out;
    append_be(out, id, id > 0xffffffU ? 4 : id > 0xffffU ? 3 : id > 0xffU ? 2 : 1);
    out.push_back(0x01);
    append_be(out, payload.size(), 7);
    append(out, payload);
    return out;
}

inline Bytes ebml_uint(std::uint32_t id, std::uint64_t value) {
    Bytes payload;
    append_be(payload, value, 8);
    return ebml(id, payload);
}

inline Bytes ebml_float(std::uint32_t id, double value) {
    std::uint64_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return ebml_uint(id, bits);
}

inline Bytes ebml_text(std::uint32_t id, std::string_view value) {
    Bytes payload;
    append_text(payload, value);
    return ebml(id, payload);
}

struct Matroska {
    std::uint64_t timestamp_scale = 1'000'000; // nanoseconds per tick
    std::optional<double> duration;            // segment Duration in ticks
    std::uint64_t default_duration = 0;        // nanoseconds per frame; zero omits the element
    std::optional<std::string_view> rotate;    // track-targeted ROTATE tag
};

// One video track with a single key frame at timestamp zero.
inline Bytes matroska(const Matroska& spec, std::span<const std::uint8_t> jpeg, int width,
                      int height) {
    Bytes header;
    append(header, ebml_uint(0x4286, 1));          // EBMLVersion
    append(header, ebml_uint(0x42f7, 1));          // EBMLReadVersion
    append(header, ebml_uint(0x42f2, 4));          // EBMLMaxIDLength
    append(header, ebml_uint(0x42f3, 8));          // EBMLMaxSizeLength
    append(header, ebml_text(0x4282, "matroska")); // DocType
    append(header, ebml_uint(0x4287, 4));          // DocTypeVersion
    append(header, ebml_uint(0x4285, 2));          // DocTypeReadVersion

    Bytes info;
    append(info, ebml_uint(0x2ad7b1, spec.timestamp_scale));
    if (spec.duration) { append(info, ebml_float(0x4489, *spec.duration)); }

    Bytes video;
    append(video, ebml_uint(0xb0, static_cast<std::uint64_t>(width)));  // PixelWidth
    append(video, ebml_uint(0xba, static_cast<std::uint64_t>(height))); // PixelHeight
    Bytes track;
    append(track, ebml_uint(0xd7, 1));         // TrackNumber
    append(track, ebml_uint(0x73c5, 1));       // TrackUID
    append(track, ebml_uint(0x83, 1));         // TrackType: video
    append(track, ebml_text(0x86, "V_MJPEG")); // CodecID
    if (spec.default_duration != 0) { append(track, ebml_uint(0x23e383, spec.default_duration)); }
    append(track, ebml(0xe0, video));

    Bytes segment;
    append(segment, ebml(0x1549a966, info));              // Info
    append(segment, ebml(0x1654ae6b, ebml(0xae, track))); // Tracks/TrackEntry
    if (spec.rotate) {
        Bytes simple;
        append(simple, ebml_text(0x45a3, "ROTATE"));     // TagName
        append(simple, ebml_text(0x4487, *spec.rotate)); // TagString
        Bytes tag;
        append(tag, ebml(0x63c0, ebml_uint(0x63c5, 1))); // Targets/TagTrackUID
        append(tag, ebml(0x67c8, simple));               // SimpleTag
        append(segment, ebml(0x1254c367, ebml(0x7373, tag)));
    }
    Bytes block = {0x81, 0x00, 0x00, 0x80}; // track 1, relative timestamp 0, key frame
    append(block, jpeg);
    Bytes cluster;
    append(cluster, ebml_uint(0xe7, 0)); // Timestamp
    append(cluster, ebml(0xa3, block));  // SimpleBlock
    append(segment, ebml(0x1f43b675, cluster));

    Bytes out = ebml(0x1a45dfa3, header);
    append(out, ebml(0x18538067, segment));
    return out;
}

inline Bytes box(std::string_view type, std::span<const std::uint8_t> payload) {
    Bytes out;
    append_be(out, 8 + payload.size(), 4);
    append_text(out, type);
    append(out, payload);
    return out;
}

constexpr std::array<std::int32_t, 9> identity_matrix = {0x10000, 0, 0, 0, 0x10000, 0,
                                                         0,       0, 0x40000000};

inline void append_matrix(Bytes& out, const std::array<std::int32_t, 9>& matrix) {
    for (const std::int32_t value : matrix) {
        append_be(out, static_cast<std::uint32_t>(value), 4);
    }
}

struct QuickTime {
    std::uint32_t timescale    = 1000; // movie and media ticks per second
    std::uint32_t duration     = 1000; // mvhd, tkhd and mdhd duration in ticks
    std::uint32_t sample_delta = 1000; // stts duration of every frame in ticks
    std::uint32_t frames       = 1;
    std::array<std::int32_t, 9> matrix = identity_matrix; // tkhd display matrix
};

// One QuickTime 'jpeg' video track whose frames form a single chunk after the movie box.
inline Bytes quicktime(const QuickTime& spec, std::span<const std::uint8_t> jpeg, int width,
                       int height) {
    Bytes brand;
    append_text(brand, "qt  ");
    append_be(brand, 0x200, 4);
    append_text(brand, "qt  ");
    const Bytes file_type = box("ftyp", brand);

    const auto movie = [&](std::uint32_t chunk_offset) {
        Bytes mvhd;
        append_zeros(mvhd, 12); // version, flags, creation and modification times
        append_be(mvhd, spec.timescale, 4);
        append_be(mvhd, spec.duration, 4);
        append_be(mvhd, 0x10000, 4); // preferred rate
        append_be(mvhd, 0x100, 2);   // preferred volume
        append_zeros(mvhd, 10);
        append_matrix(mvhd, identity_matrix);
        append_zeros(mvhd, 24); // preview, poster, selection and current times
        append_be(mvhd, 2, 4);  // next track ID

        Bytes tkhd;
        append_be(tkhd, 3, 4);  // version 0; enabled, in movie
        append_zeros(tkhd, 8);  // creation and modification times
        append_be(tkhd, 1, 4);  // track ID
        append_zeros(tkhd, 4);
        append_be(tkhd, spec.duration, 4);
        append_zeros(tkhd, 16); // reserved, layer, alternate group, volume, reserved
        append_matrix(tkhd, spec.matrix);
        append_be(tkhd, static_cast<std::uint64_t>(width) << 16U, 4);
        append_be(tkhd, static_cast<std::uint64_t>(height) << 16U, 4);

        Bytes mdhd;
        append_zeros(mdhd, 12); // version, flags, creation and modification times
        append_be(mdhd, spec.timescale, 4);
        append_be(mdhd, spec.duration, 4);
        append_zeros(mdhd, 4); // language and quality

        Bytes hdlr;
        append_zeros(hdlr, 4);
        append_text(hdlr, "mhlr");
        append_text(hdlr, "vide");
        append_zeros(hdlr, 13); // manufacturer, flags, flags mask, empty Pascal name

        Bytes entry;
        append_zeros(entry, 6);          // reserved
        append_be(entry, 1, 2);          // data reference index
        append_zeros(entry, 12);         // version, revision, vendor, temporal quality
        append_be(entry, 0x200, 4);      // spatial quality
        append_be(entry, static_cast<std::uint64_t>(width), 2);
        append_be(entry, static_cast<std::uint64_t>(height), 2);
        append_be(entry, 0x480000, 4);   // horizontal resolution
        append_be(entry, 0x480000, 4);   // vertical resolution
        append_zeros(entry, 4);          // data size
        append_be(entry, 1, 2);          // frames per sample
        append_zeros(entry, 32);         // empty Pascal compressor name
        append_be(entry, 24, 2);         // depth
        append_be(entry, 0xffff, 2);     // no color table
        Bytes stsd;
        append_zeros(stsd, 4);
        append_be(stsd, 1, 4);
        append(stsd, box("jpeg", entry));

        Bytes stts;
        append_zeros(stts, 4);
        append_be(stts, 1, 4);
        append_be(stts, spec.frames, 4);
        append_be(stts, spec.sample_delta, 4);
        Bytes stsc;
        append_zeros(stsc, 4);
        append_be(stsc, 1, 4);
        append_be(stsc, 1, 4); // first chunk
        append_be(stsc, spec.frames, 4);
        append_be(stsc, 1, 4); // sample description index
        Bytes stsz;
        append_zeros(stsz, 4);
        append_be(stsz, jpeg.size(), 4); // every sample has the same size
        append_be(stsz, spec.frames, 4);
        Bytes stco;
        append_zeros(stco, 4);
        append_be(stco, 1, 4);
        append_be(stco, chunk_offset, 4);

        Bytes stbl;
        append(stbl, box("stsd", stsd));
        append(stbl, box("stts", stts));
        append(stbl, box("stsc", stsc));
        append(stbl, box("stsz", stsz));
        append(stbl, box("stco", stco));
        Bytes vmhd;
        append_be(vmhd, 1, 4); // version 0, flags 1
        append_zeros(vmhd, 8); // graphics mode and opcolor
        Bytes minf;
        append(minf, box("vmhd", vmhd));
        append(minf, box("stbl", stbl));
        Bytes mdia;
        append(mdia, box("mdhd", mdhd));
        append(mdia, box("hdlr", hdlr));
        append(mdia, box("minf", minf));
        Bytes trak;
        append(trak, box("tkhd", tkhd));
        append(trak, box("mdia", mdia));
        Bytes moov;
        append(moov, box("mvhd", mvhd));
        append(moov, box("trak", trak));
        return box("moov", moov);
    };

    // The chunk starts after ftyp, moov and the mdat header; moov's size does not depend on it.
    const std::size_t movie_size = movie(0).size();
    Bytes out                    = file_type;
    append(out, movie(static_cast<std::uint32_t>(file_type.size() + movie_size + 8)));
    Bytes samples;
    for (std::uint32_t frame = 0; frame < spec.frames; ++frame) { append(samples, jpeg); }
    append(out, box("mdat", samples));
    return out;
}

} // namespace container_fixtures
