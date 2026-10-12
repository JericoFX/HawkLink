#pragma once
#include "hawken/game_wire.hpp"

namespace hawken::wire {
// Emits one interior packet: ID14, ACK/bunch records in LSB order and final
// marker1 with zero byte padding, recovered from9f3410/9f2760/9e4480.
// Bunch.control names flagsPresent: native9f3410 derives it as open||close.
// No reservation/increment of IDs or sequences, default endian, or sockets.
//
// Our bounded/canonical API policy (not recovered native input restrictions):
// max_packet_bytes must be1..65507 and includes final marker/padding. 65507 is
// an interior buffer ceiling, not a native limit or a UDP-size guarantee after
// adding the48-byte session wrapper; transport must budget overhead separately.
// All records and total bit capacity are validated before BitWriter allocation.
// ACK requires default/empty Bunch; nonACK requires ack_packet_id0. Absent
// sequence/type are0; control==(open||close). Payload storage is exactly
// ceil(payload_bits/8), with unused high bits0; payload_bits<max_packet_bytes*8.
// Empty records are valid if header+marker fit. No interval/gameplay policy.
// WireError.code is fixed: invalid_limit/value/length, inconsistent_record,
// packet_too_large, or not_implemented for the RED stub; never payload content.
std::vector<std::uint8_t> encode_packet(const Packet& packet, std::uint32_t max_packet_bytes);
}
