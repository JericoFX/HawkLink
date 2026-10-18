#include "hawken/weapon_fire_rpc.hpp"
namespace hawken::game::weapon_fire {
Result read_rpc(wire::BitReader&,const Schema&,std::size_t) { throw wire::WireError("invalid_value"); }
}
