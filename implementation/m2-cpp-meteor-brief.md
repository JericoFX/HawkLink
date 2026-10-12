# M2 — contratos Meteor en C++

Ownership siguiente de auth_endpoints: include/hawken/meteor_compat.hpp, src/meteor_compat.cpp, tests/meteor_compat_tests.cpp, implementation/m2-cpp-meteor-report.md. Esperar aceptación M1 antes de tocar nuevos archivos; preparar primero modelos/API, stubs y tests, avisar congelado para RED. No estás solo: preservar cambios de padre/game_master. No builds/tests/Git/delegación ni nuevas dependencias; padre integra CMake. C++20, nlohmann/doctest actuales.

Objetivo implementable: contratos tipados de las respuestas del directorio y sesión, más envelope y predicado de éxito. Esto no verifica Steam, admisión, región UI, sesión emitida ni gameplay. No fabricar grants/tickets ni implementar otra autenticación.

Evidencia primaria: investigación/HAWKEN_MASTER_SERVER_PROTOCOL.md, evidence/auth/recovered-contracts.json y ghidra-sdk-schemas/results/services.jsonl. SDK original SHA f63fef02ffb517ca7844a769d03412ad82bd22198397302ee3c090c63a970c57. Parser listing RVA7a610/raw79a10. Session parser84da0→84f40. Envelope708a0 escribe Status en objeto+410 y exige Message; Result se serializa como valor JSON.

Nueva evidencia comprobada por padre: implementation/evidence/m2-sdk-results/sdk-results.jsonl. Exports Success26130/raw25530 y Error26170: éxito requiere campos +c y +410 entre200–299, y +814 igual0 o200–299. HttpError261c0 evalúa+c; SetSuccess261d0 escribe200 en los3. No asumir flags0/1. Correlación +410↔envelope demostrada por708a0; +814 errores de parse fijados500. Layout/nombres originales de clase no se recuperaron por completo.

API namespace hawken::meteor:
- bool result_success(std::uint32_t http_status,std::uint32_t api_status,std::uint32_t parse_status);
- nlohmann::json envelope(std::uint32_t status,std::string_view message,const nlohmann::json& result). Message siempre string. Mantener Result como valor, no doble encoding. No es un ticket/servicio de auth.
- struct GameServerListing con nombres C++ snake_case y tipos: strings guid,match_id,region,game_version,server_name,map,game_type,endpoint,port,voice_channel_listing; int32 max_users,min_users,server_score,server_ranking,match_completion_percent; bool is_matchmaking_visible,is_public_visible; vector<string> users,allowed_roles; map<string,string> developer_data. Defaults0/false y strings vacías. json listing_json(const GameServerListing&).
- struct ClientSession {strings user_guid,session_id,session_key,assigned_server_ip,assigned_server_guid,client_advertisement_guid; int32 assigned_server_port;}; json session_json(const ClientSession&). No logging del contenido, política de creación aparte.
- std::string escape_component(std::string_view utf8); safe bytes exactos -.0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ_abcdefghijklmnopqrstuvwxyz~ de evidence/auth/uri-escape-whitelist.json, encode otros bytes %HH mayúscula, no re-encode componentes ya decodificados automáticamente. Input % ya codificado se trata literal y se escapa una vez al construir un componente nuevo.

Tests no triviales: casing exacto y todos los tipos de listing frente a keys/tipos estáticos; Users/AllowedRoles arrays strings, DeveloperData valuesstrings, Port string distinta del AssignedServerPort int; Result objeto/array/string/null preservado; Message requerido aunque vacío; predicado bordes199/200/299/300, parse0/200/299/500 y Status0 falla; escape Unicodebytes/espacios/+/%/?/#/slash vs safe, sin normalizar ni doble decode. Las salidas fixture no prueban consumidor original en ejecución. Sin parsers extra ni conveniencia especulativa.

Tras RED sólo implementar APIs observadas. Reportar límites y fuentes/RVA/hash en informe breve; verificar por lectura completa antes de congelar. CMake y SHA/CLI son del padre.
