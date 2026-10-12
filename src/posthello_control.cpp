#include "hawken/posthello_control.hpp"
#include <bit>
#include <limits>
#include <new>
#include <type_traits>
#include <utility>

namespace {
using hawken::control::Endian;
using namespace hawken::control::posthello;
const char* error_message(ErrorCode code) noexcept {
    switch (code) {
    case ErrorCode::invalid_endian: return "invalid post-Hello endian";
    case ErrorCode::invalid_limits: return "invalid post-Hello limits";
    case ErrorCode::invalid_message: return "invalid post-Hello message";
    case ErrorCode::unsupported_opcode: return "unsupported post-Hello opcode";
    case ErrorCode::truncated_archive: return "truncated post-Hello archive";
    case ErrorCode::extra_bytes: return "extra post-Hello archive bytes";
    case ErrorCode::resource_limit: return "post-Hello resource limit";
    case ErrorCode::not_implemented: return "post-Hello codec unavailable";
    }
    return "post-Hello codec failed";
}

void check_configuration(Endian endian, Limits limits) {
    if (endian != Endian::little && endian != Endian::big)
        throw Error(ErrorCode::invalid_endian);
    if (limits.max_archive_bytes == 0 || limits.max_string_units == 0 ||
        limits.max_string_units > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max()))
        throw Error(ErrorCode::invalid_limits);
}

std::size_t body_bytes(std::size_t units, StringForm form) {
    const std::size_t width=form == StringForm::wide ? 2 : 1;
    if (units > std::numeric_limits<std::size_t>::max()/width)
        throw Error(ErrorCode::resource_limit);
    return units*width;
}

class Sizer {
public:
    explicit Sizer(Limits limits) : limits_(limits) {}
    void byte(std::uint8_t) { add(1); }
    void u32(std::uint32_t) { add(4); }
    void string(const RawString& value) {
        switch (value.form) {
        case StringForm::zero:
            if (!value.units.empty()) throw Error(ErrorCode::invalid_message);
            add(4);
            return;
        case StringForm::narrow:
        case StringForm::wide:
            if (value.units.empty()) throw Error(ErrorCode::invalid_message);
            break;
        default: throw Error(ErrorCode::invalid_message);
        }
        if (value.units.size() > limits_.max_string_units)
            throw Error(ErrorCode::resource_limit);
        add(4);
        add(body_bytes(value.units.size(),value.form));
        if (value.form == StringForm::narrow) {
            for (const char16_t unit:value.units)
                if (unit > 0xff) throw Error(ErrorCode::invalid_message);
        }
    }
    std::size_t size() const noexcept { return size_; }
private:
    void add(std::size_t bytes) {
        if (bytes > limits_.max_archive_bytes-size_)
            throw Error(ErrorCode::resource_limit);
        size_+=bytes;
    }
    Limits limits_;
    std::size_t size_=0;
};

class Writer {
public:
    Writer(std::vector<std::uint8_t>& bytes, Endian endian) : bytes_(bytes), endian_(endian) {}
    void byte(std::uint8_t value) { bytes_.push_back(value); }
    void u32(std::uint32_t value) {
        for (unsigned i=0; i<4; ++i) {
            const unsigned shift=(endian_ == Endian::little ? i : 3-i)*8;
            byte(static_cast<std::uint8_t>(value >> shift));
        }
    }
    // Complete Sizer preflight already validated form, count and total bytes.
    void string(const RawString& value) {
        const auto count=static_cast<std::int32_t>(value.units.size());
        const std::int32_t signed_count=value.form == StringForm::wide ? -count : count;
        u32(std::bit_cast<std::uint32_t>(signed_count));
        for (const char16_t unit:value.units) {
            byte(static_cast<std::uint8_t>(unit));
            if (value.form == StringForm::wide)
                byte(static_cast<std::uint8_t>(unit >> 8));
        }
    }
private:
    std::vector<std::uint8_t>& bytes_;
    Endian endian_;
};

// The same field traversal is used for sizing and emission; sizing performs
// all validation before the output buffer is reserved or any payload copied.
template<class Sink>
void serialize(const Message& message, Sink& sink) {
    std::visit([&](const auto& value) {
        using T=std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T,Welcome>) {
            sink.byte(1);
            sink.string(value.level);
            sink.string(value.game);
        } else if constexpr (std::is_same_v<T,Challenge>) {
            sink.byte(3);
            sink.u32(value.version);
            sink.string(value.challenge);
        } else if constexpr (std::is_same_v<T,NetSpeed>) {
            sink.byte(4);
            sink.u32(value.value);
        } else if constexpr (std::is_same_v<T,Login>) {
            sink.byte(5);
            sink.string(value.response);
            sink.string(value.url);
            for (const auto word:value.unique_id) sink.u32(word);
        } else if constexpr (std::is_same_v<T,Failure>) {
            sink.byte(6);
            sink.string(value.error);
        } else if constexpr (std::is_same_v<T,Package>) {
            sink.byte(7);
            for (const auto word:value.identity) sink.u32(word);
            sink.string(value.name0);
            sink.string(value.name1);
            sink.string(value.text2);
            sink.u32(value.value0);
            sink.u32(value.value1);
            sink.string(value.name3);
            sink.byte(value.flags);
        } else if constexpr (std::is_same_v<T,DownloadMetadata>) {
            sink.byte(0x0b);
            sink.string(value.class_path);
            sink.string(value.parameters);
            sink.u32(value.value);
        } else if constexpr (std::is_same_v<T,PackageAck>) {
            sink.byte(8);
            for (const auto word:value.identity) sink.u32(word);
            sink.u32(value.generation);
        } else if constexpr (std::is_same_v<T,Join>) {
            sink.byte(9);
        }
    },message);
}

struct StringView {
    StringForm form=StringForm::zero;
    std::size_t units=0;
    std::span<const std::uint8_t> body;
};

class Reader {
public:
    Reader(std::span<const std::uint8_t> bytes, Endian endian, Limits limits)
        : bytes_(bytes), endian_(endian), limits_(limits) {}
    std::uint8_t byte() { return take(1)[0]; }
    std::uint32_t u32() {
        const auto bytes=take(4);
        std::uint32_t result=0;
        for (unsigned i=0; i<4; ++i) {
            const unsigned shift=(endian_ == Endian::little ? i : 3-i)*8;
            result |= static_cast<std::uint32_t>(bytes[i]) << shift;
        }
        return result;
    }
    StringView string() {
        const auto count=std::bit_cast<std::int32_t>(u32());
        if (count == std::numeric_limits<std::int32_t>::min())
            throw Error(ErrorCode::invalid_message);
        const auto units=static_cast<std::size_t>(count < 0 ? -count : count);
        if (units > limits_.max_string_units)
            throw Error(ErrorCode::resource_limit);
        const auto form=count == 0 ? StringForm::zero : count < 0 ? StringForm::wide : StringForm::narrow;
        return {form,units,take(body_bytes(units,form))};
    }
    std::size_t position() const noexcept { return position_; }
private:
    std::span<const std::uint8_t> take(std::size_t bytes) {
        // Check the first message's consumed-byte budget, not span.size(). A
        // later message in the span is deliberately neither read nor budgeted.
        if (bytes > limits_.max_archive_bytes-position_)
            throw Error(ErrorCode::resource_limit);
        if (bytes > bytes_.size()-position_)
            throw Error(ErrorCode::truncated_archive);
        const auto result=bytes_.subspan(position_,bytes);
        position_+=bytes;
        return result;
    }
    std::span<const std::uint8_t> bytes_;
    Endian endian_;
    Limits limits_;
    std::size_t position_=0;
};

// Stack-only parse descriptors retain input spans. The full message is parsed
// and budgeted before materializing any dynamically allocated RawString units.
struct Parsed {
    std::uint8_t opcode;
    std::array<std::uint32_t,4> identity{};
    std::array<StringView,4> strings{};
    std::uint32_t value0=0;
    std::uint32_t value1=0;
    std::uint8_t flags=0;
};

Parsed parse(Reader& reader) {
    Parsed result{reader.byte()};
    switch (result.opcode) {
    case 1:
        result.strings[0]=reader.string();
        result.strings[1]=reader.string();
        break;
    case 3:
        result.value0=reader.u32();
        result.strings[0]=reader.string();
        break;
    case 4:
        result.value0=reader.u32();
        break;
    case 5:
        result.strings[0]=reader.string();
        result.strings[1]=reader.string();
        for (auto& word:result.identity) word=reader.u32();
        break;
    case 6:
        result.strings[0]=reader.string();
        break;
    case 7:
        for (auto& word:result.identity) word=reader.u32();
        result.strings[0]=reader.string();
        result.strings[1]=reader.string();
        result.strings[2]=reader.string();
        result.value0=reader.u32();
        result.value1=reader.u32();
        result.strings[3]=reader.string();
        result.flags=reader.byte();
        break;
    case 8:
        for (auto& word:result.identity) word=reader.u32();
        result.value0=reader.u32();
        break;
    case 9:
        break;
    case 0x0b:
        result.strings[0]=reader.string();
        result.strings[1]=reader.string();
        result.value0=reader.u32();
        break;
    default: throw Error(ErrorCode::unsupported_opcode);
    }
    return result;
}

RawString materialize(const StringView& value) {
    RawString result{value.form,{}};
    if (value.units > result.units.max_size())
        throw Error(ErrorCode::resource_limit);
    result.units.resize(value.units);
    for (std::size_t i=0; i<value.units; ++i) {
        if (value.form == StringForm::wide) {
            const auto unit=static_cast<std::uint16_t>(value.body[i*2]) |
                            (static_cast<std::uint16_t>(value.body[i*2+1]) << 8);
            result.units[i]=static_cast<char16_t>(unit);
        } else {
            result.units[i]=static_cast<char16_t>(value.body[i]);
        }
    }
    return result;
}

Message materialize(const Parsed& value) {
    switch (value.opcode) {
    case 1: return Welcome{materialize(value.strings[0]),materialize(value.strings[1])};
    case 3: return Challenge{value.value0,materialize(value.strings[0])};
    case 4: return NetSpeed{value.value0};
    case 5: return Login{materialize(value.strings[0]),materialize(value.strings[1]),value.identity};
    case 6: return Failure{materialize(value.strings[0])};
    case 7: return Package{value.identity,materialize(value.strings[0]),materialize(value.strings[1]),
                          materialize(value.strings[2]),value.value0,value.value1,
                          materialize(value.strings[3]),value.flags};
    case 8: return PackageAck{value.identity,value.value0};
    case 9: return Join{};
    case 0x0b: return DownloadMetadata{materialize(value.strings[0]),
                                    materialize(value.strings[1]),value.value0};
    default: throw Error(ErrorCode::unsupported_opcode);
    }
}

Decoded decode_message(std::span<const std::uint8_t> archive, Endian endian,
                       Limits limits, bool exact) {
    check_configuration(endian,limits);
    Reader reader(archive,endian,limits);
    const auto parsed=parse(reader);
    // Exact-mode suffix rejection also precedes string allocation. Prefix
    // leaves the suffix untouched, regardless of its length or contents.
    if (exact && reader.position() != archive.size()) throw Error(ErrorCode::extra_bytes);
    try {
        return {materialize(parsed),reader.position()};
    } catch (const std::bad_alloc&) {
        throw Error(ErrorCode::resource_limit);
    } catch (const std::length_error&) {
        throw Error(ErrorCode::resource_limit);
    }
}
}

namespace hawken::control::posthello {
Error::Error(ErrorCode code) : std::runtime_error(error_message(code)), code_(code) {}
ErrorCode Error::code() const noexcept { return code_; }

std::vector<std::uint8_t> encode(const Message& message, Endian endian, Limits limits) {
    check_configuration(endian,limits);
    if (message.valueless_by_exception()) throw Error(ErrorCode::invalid_message);
    Sizer size(limits);
    serialize(message,size);
    try {
        std::vector<std::uint8_t> bytes;
        if (size.size() > bytes.max_size()) throw Error(ErrorCode::resource_limit);
        bytes.reserve(size.size());
        Writer writer(bytes,endian);
        serialize(message,writer);
        return bytes;
    } catch (const std::bad_alloc&) {
        throw Error(ErrorCode::resource_limit);
    } catch (const std::length_error&) {
        throw Error(ErrorCode::resource_limit);
    }
}
Decoded decode_prefix(std::span<const std::uint8_t> archive, Endian endian, Limits limits) {
    return decode_message(archive,endian,limits,false);
}
Message decode_exact(std::span<const std::uint8_t> archive, Endian endian, Limits limits) {
    auto decoded=decode_message(archive,endian,limits,true);
    return std::move(decoded.message);
}
}
