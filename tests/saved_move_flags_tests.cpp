#include "hawken/saved_move_flags.hpp"
#include <doctest/doctest.h>
#include <array>
#include <cstdint>
#include <ostream>
#include <string_view>

namespace {
using hawken::wire::movement::ScopedSavedMoveDirection;
using hawken::wire::movement::decode_hawken_saved_move_flags;
}

TEST_CASE("Hawken saved move flags decode all 256 bytes with original direction fallback") {
    // Original R_SavedMove.SetFlags: export40618, serial0xc2b95c,
    // UC044347 SHA d57b1d98c86ffc2384a9fffc4f163491f27674fb6295091bd3bf12e29dc11bf5.
    // Independent literal truth columns for the32 high-five-bit combinations,
    // ordered run/heal/jump/taunt/precise. No decoder computes expected values.
    constexpr std::array<std::string_view,5> truth{{
        "01010101010101010101010101010101",
        "00110011001100110011001100110011",
        "00001111000011110000111100001111",
        "00000000111111110000000011111111",
        "00000000000000001111111111111111"
    }};
    constexpr std::array<ScopedSavedMoveDirection,8> directions{{
        ScopedSavedMoveDirection::none,ScopedSavedMoveDirection::left,
        ScopedSavedMoveDirection::right,ScopedSavedMoveDirection::forward,
        ScopedSavedMoveDirection::back,ScopedSavedMoveDirection::none,
        ScopedSavedMoveDirection::none,ScopedSavedMoveDirection::none
    }};
    for(std::size_t group=0;group<32;++group) {
        for(std::size_t low=0;low<8;++low) {
            const auto raw=static_cast<std::uint8_t>(group*8+low);CAPTURE(static_cast<unsigned>(raw));
            const auto value=decode_hawken_saved_move_flags(raw);
            CHECK(value.run==(truth[0][group]=='1'));
            CHECK(value.heal==(truth[1][group]=='1'));
            CHECK(value.pressed_jump==(truth[2][group]=='1'));
            CHECK(value.taunt==(truth[3][group]=='1'));
            CHECK_FALSE(value.double_jump);
            CHECK(value.precise_destination==(truth[4][group]=='1'));
            CHECK(value.direction==directions[low]);
        }
    }
}

TEST_CASE("Hawken saved move flags preserve heal taunt and jump instead of Engine double jump") {
    struct Example {
        std::uint8_t raw;
        bool run,heal,jump,taunt,precise;
        ScopedSavedMoveDirection direction;
    };
    constexpr std::array<Example,8> examples{{
        {0x10,false,true,false,false,false,ScopedSavedMoveDirection::none},
        {0x40,false,false,false,true,false,ScopedSavedMoveDirection::none},
        {0x60,false,false,true,true,false,ScopedSavedMoveDirection::none},
        {0x50,false,true,false,true,false,ScopedSavedMoveDirection::none},
        {0xff,true,true,true,true,true,ScopedSavedMoveDirection::none},
        {0xa3,false,false,true,false,true,ScopedSavedMoveDirection::forward},
        {0x22,false,false,true,false,false,ScopedSavedMoveDirection::right},
        {0x1d,true,true,false,false,false,ScopedSavedMoveDirection::none}
    }};
    for(const auto& example:examples) {
        CAPTURE(static_cast<unsigned>(example.raw));
        const auto value=decode_hawken_saved_move_flags(example.raw);
        CHECK(value.run==example.run);CHECK(value.heal==example.heal);
        CHECK(value.pressed_jump==example.jump);CHECK(value.taunt==example.taunt);
        CHECK_FALSE(value.double_jump);CHECK(value.precise_destination==example.precise);
        CHECK(value.direction==example.direction);
    }
    // Constexpr is part of the pure API; behavior remains a runtime assertion
    // so the temporary empty RED implementation compiles and fails semantically.
    constexpr auto constant_jump=decode_hawken_saved_move_flags(0x20);
    CHECK(constant_jump.pressed_jump);
}
