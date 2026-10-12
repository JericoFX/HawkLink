#include "hawken/package_publication.hpp"
#include <limits>
#include <new>
#include <utility>

namespace hawken::control::packages {
posthello::Package make_root_package(const RootMetadata& metadata, posthello::Limits limits) {
    using namespace posthello;
    const auto max_count=static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max());
    if (limits.max_archive_bytes == 0 || limits.max_string_units == 0 ||
        limits.max_string_units > max_count)
        throw Error(ErrorCode::invalid_limits);

    if (metadata.name.empty() || metadata.generation_net_counts.empty() ||
        metadata.generation_net_counts.size() > max_count ||
        (metadata.identity[0] | metadata.identity[1] | metadata.identity[2] | metadata.identity[3]) == 0)
        throw Error(ErrorCode::invalid_message);
    if (metadata.name.size() == 4 &&
        (metadata.name[0] == 'N' || metadata.name[0] == 'n') &&
        (metadata.name[1] == 'O' || metadata.name[1] == 'o') &&
        (metadata.name[2] == 'N' || metadata.name[2] == 'n') &&
        (metadata.name[3] == 'E' || metadata.name[3] == 'e'))
        throw Error(ErrorCode::invalid_message);
    for (const char byte:metadata.name) {
        if (!((byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z') ||
              (byte >= '0' && byte <= '9') || byte == '_' || byte == '-'))
            throw Error(ErrorCode::invalid_message);
    }

    // For this explicit representation: opcode/GUID/scalars/counts/None bodies
    // and root terminator total45 bytes, plus the ASCII root name. Check before
    // allocating either the UTF16 units or the codec's output validation buffer.
    if (metadata.name.size() >= limits.max_string_units ||
        limits.max_archive_bytes < 45 || metadata.name.size() > limits.max_archive_bytes-45)
        throw Error(ErrorCode::resource_limit);
    try {
        RawString name{StringForm::narrow,{}};
        name.units.reserve(metadata.name.size()+1);
        for (const char byte:metadata.name) name.units.push_back(static_cast<char16_t>(byte));
        name.units.push_back(u'\0');
        Package result{metadata.identity,std::move(name),
            {StringForm::narrow,std::u16string(1,u'\0')},
            {StringForm::zero,{}},0,
            static_cast<std::uint32_t>(metadata.generation_net_counts.size()),
            {StringForm::narrow,std::u16string(1,u'\0')},0};
        // Reuse the complete codec validation; scalar endian cannot affect size
        // or representation validity. No encoded archive is retained here.
        (void)encode(result,Endian::little,limits);
        return result;
    } catch (const std::bad_alloc&) {
        throw Error(ErrorCode::resource_limit);
    } catch (const std::length_error&) {
        throw Error(ErrorCode::resource_limit);
    }
}

posthello::Package make_contained_package(const ContainedMetadata& metadata, posthello::Limits limits) {
    using namespace posthello;
    const auto max_count=static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max());
    if (limits.max_archive_bytes == 0 || limits.max_string_units == 0 ||
        limits.max_string_units > max_count)
        throw Error(ErrorCode::invalid_limits);

    const auto& container=metadata.container_name;
    if (container.empty()) throw Error(ErrorCode::invalid_message);
    for (const char byte:container) {
        if (!((byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z') ||
              (byte >= '0' && byte <= '9') || byte == '_' || byte == '-'))
            throw Error(ErrorCode::invalid_message);
    }
    const auto fold=[](char byte) {
        return byte >= 'A' && byte <= 'Z' ? static_cast<char>(byte+'a'-'A') : byte;
    };
    if (container.size() == 4 && fold(container[0]) == 'n' &&
        fold(container[1]) == 'o' && fold(container[2]) == 'n' && fold(container[3]) == 'e')
        throw Error(ErrorCode::invalid_message);
    const auto& name=metadata.package.name;
    bool equal=name.size() == container.size();
    for (std::size_t i=0;equal && i<name.size();++i)
        equal=fold(name[i]) == fold(container[i]);
    if (equal) throw Error(ErrorCode::invalid_message);

    // Replacing root name3's single NUL with container+NUL adds its ASCII
    // length. Check the full45+name+container budget without overflowing or
    // allocating; both string limits include their terminators.
    if (name.size() >= limits.max_string_units || container.size() >= limits.max_string_units ||
        limits.max_archive_bytes < 45 || name.size() > limits.max_archive_bytes-45 ||
        container.size() > limits.max_archive_bytes-45-name.size())
        throw Error(ErrorCode::resource_limit);
    try {
        // Reuse root metadata validation and explicit local-only fields. No
        // inferred historical export flags, linker association or downloads.
        auto result=make_root_package(metadata.package,limits);
        RawString container_name{StringForm::narrow,{}};
        container_name.units.reserve(container.size()+1);
        for (const char byte:container) container_name.units.push_back(static_cast<char16_t>(byte));
        container_name.units.push_back(u'\0');
        result.name3=std::move(container_name);
        (void)encode(result,Endian::little,limits);
        return result;
    } catch (const std::bad_alloc&) {
        throw Error(ErrorCode::resource_limit);
    } catch (const std::length_error&) {
        throw Error(ErrorCode::resource_limit);
    }
}
}
