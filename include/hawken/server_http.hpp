#pragma once

namespace httplib { class Server; }
namespace hawken::auth { class AuthService; }
namespace hawken::sync { class SyncService; class MenuSyncService; }
namespace hawken::inventory { class InventoryService; }
namespace hawken::assignment { class SessionAssignmentService; }
namespace hawken::master { class MasterDirectory; }
namespace hawken::server {
// Installs startup/version replies for the server backend, separate from the Master.
// The optional verified local facade supplies login/profile/logout only.
// Without either facade, startup-only replies and unread login bodies remain.
// The facade must outlive the listener; no menu/game admission is provided.
void configure_server_http(httplib::Server& server);
void configure_server_http(httplib::Server& server, auth::AuthService* auth);
// Optional initial sync reads. Both services must outlive the listener.
void configure_server_http(httplib::Server& server, auth::AuthService* auth, sync::SyncService* sync);
// Optional protected type/offer/owned catalogue reads. All supplied facades
// must outlive the listener; they provide no GameServer admission or gameplay.
void configure_server_http(httplib::Server& server, auth::AuthService* auth,
                           sync::SyncService* sync, inventory::InventoryService* inventory);
// Optional remaining initial-sync GETs. HTTP and Meteor API status are separate
// for the original unlocked-resource absence branch. Services outlive listener.
void configure_server_http(httplib::Server& server, auth::AuthService* auth,
                           sync::SyncService* sync, inventory::InventoryService* inventory,
                           sync::MenuSyncService* menu);
// Optional session/legacy assignment facade. Its confirmed UDP lease belongs
// to the server owner; supplying this facade alone proves no socket or admission.
// Every facade must outlive the listener and all in-flight request handlers.
void configure_server_http(httplib::Server& server, auth::AuthService* auth,
                           sync::SyncService* sync, inventory::InventoryService* inventory,
                           sync::MenuSyncService* menu,
                           assignment::SessionAssignmentService* assignment);
// Optional read-only listing snapshot for the client's shared Meteor API base.
// Collection reads disclose only directory metadata. A successful individual
// lookup selects through the SERVER assignment facade when supplied; the Master
// itself never authenticates or issues a session. No remote-directory fetch,
// host advertisement, world readiness or game admission is implied.
void configure_server_http(httplib::Server& server, auth::AuthService* auth,
                           sync::SyncService* sync, inventory::InventoryService* inventory,
                           sync::MenuSyncService* menu,
                           assignment::SessionAssignmentService* assignment,
                           const master::MasterDirectory* directory);
}
