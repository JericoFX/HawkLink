# M3 LocalStore — implementación y preparación GREEN

Estado: lógica implementada tras RED externo revisado por el padre; fuente y pruebas preparadas para su revisión y GREEN externo. No se han ejecutado builds ni tests por este escritor. La aceptación corresponde al padre.

Propiedad: `include/hawken/local_store.hpp`, `src/local_store.cpp`, `tests/local_store_tests.cpp` y este informe. CMake/dependencia pertenecen al padre; la consulta readonly pertenece al otro escritor. La validación compartida tiene dueño externo.

## Contrato implementado

`hawken::store::LocalStore` abre una DB por `server_id`. Expone `schema_version`, `get_or_create_account`, `find_account`, `record_visit`, `aliases`, `visits`, `set_role`, `role`, `set_ban`, `ban` e `is_banned`. `record_visit` devuelve true al insertar y false para replay idéntico; reutilizar `visit_id` con otro account, alias o timestamp produce `conflict`, sin cambiar historial ni aliases. `set_role(nullopt)` revoca y `role` devuelve optional; ninguna cuenta recibe OWNER automáticamente.

Modelos públicos: `AccountRef`, `Account`, `Alias`, `Visit`, `Role` y `Ban`. La SteamID64 se almacena como TEXT decimal canónico, no cero, dentro de uint64. La ID local es un entero independiente. `AccountRef` limita el scope lógico; no identifica de forma segura clones con el mismo `server_id`. Esta API interna exige autenticación y autorización del caller; almacenar un vínculo no verifica Steam ni autoriza admisión.

IDs de servidor y visita: ASCII `[A-Za-z0-9_.-]`, 1..64 bytes, excluyendo `.` y `..`. Alias: UTF-8 válido, 1..128 bytes. Reason: UTF-8 válido, 0..256 bytes. Ambos rechazan controles C0/DEL/C1, incluyendo NUL. Timestamps no negativos; expiration, si existe, estrictamente mayor que starts. Ban activo en `[starts, expires)`; el caller proporciona `now`. Lecturas con límite 1..256; visitas ordenadas por occurred_at descendente / visit_id ascendente y aliases por last_seen descendente / value ascendente.

`StoreError` conserva un enum propio y el mensaje fijo `Local store operation failed.`; no copia errmsg SQLite, paths o valores. Códigos: invalid_input, invalid_scope, not_found, conflict, unsupported_schema, invalid_schema, busy, io_error. Los stubs de RED arrojaban explícitamente `std::logic_error("LocalStore not implemented")`; ahora las operaciones utilizan SQLite real, prepare/bind y transacciones. No hay mocks ni handle público.

## DDL exacto v1 aprobado

Application ID aceptado: `0x4841574B`; `user_version=1`. Seis tablas STRICT, sin views/triggers ni SQL del usuario:

| Tabla | Columnas y constraints previstas |
|---|---|
| server_metadata | singleton INTEGER PRIMARY KEY CHECK(singleton=1), server_id TEXT NOT NULL; exactamente una fila con el servidor solicitado |
| accounts | id INTEGER PRIMARY KEY positivo, steam_id64 TEXT NOT NULL UNIQUE con forma/rango decimal, created_at INTEGER NOT NULL CHECK(created_at>=0) |
| aliases | account_id INTEGER NOT NULL FK accounts(id), value TEXT NOT NULL con tamaño 1..128 bytes, first_seen/last_seen INTEGER NOT NULL con 0<=first_seen<=last_seen, PRIMARY KEY(account_id,value) |
| visits | visit_id TEXT PRIMARY KEY NOT NULL con tamaño/ASCII seguro, account_id INTEGER NOT NULL FK accounts(id), alias TEXT NOT NULL con tamaño 1..128 bytes, occurred_at INTEGER NOT NULL CHECK(occurred_at>=0) |
| roles | account_id INTEGER PRIMARY KEY FK accounts(id), role TEXT NOT NULL CHECK(role IN ('owner','moderator','analyst')) |
| bans | account_id INTEGER PRIMARY KEY FK accounts(id), starts_at INTEGER NOT NULL CHECK(starts_at>=0), expires_at INTEGER nullable CHECK(expires_at IS NULL OR expires_at>starts_at), reason TEXT NOT NULL con tamaño <=256 bytes |

```sql
CREATE TABLE server_metadata (
    singleton INTEGER PRIMARY KEY NOT NULL CHECK(singleton = 1),
    server_id TEXT NOT NULL CHECK(
        length(CAST(server_id AS BLOB)) BETWEEN 1 AND 64
        AND instr(CAST(server_id AS BLOB), x'00') = 0
        AND server_id NOT GLOB '*[^A-Za-z0-9_.-]*'
        AND server_id NOT IN ('.', '..'))
) STRICT;

CREATE TABLE accounts (
    id INTEGER PRIMARY KEY NOT NULL CHECK(id > 0),
    steam_id64 TEXT NOT NULL UNIQUE CHECK(
        length(steam_id64) BETWEEN 1 AND 20
        AND instr(CAST(steam_id64 AS BLOB), x'00') = 0
        AND steam_id64 NOT GLOB '*[^0-9]*'
        AND substr(steam_id64, 1, 1) BETWEEN '1' AND '9'
        AND (length(steam_id64) < 20
             OR steam_id64 COLLATE BINARY <= '18446744073709551615')),
    created_at INTEGER NOT NULL CHECK(created_at >= 0)
) STRICT;

CREATE TABLE aliases (
    account_id INTEGER NOT NULL REFERENCES accounts(id) ON DELETE RESTRICT,
    value TEXT NOT NULL CHECK(
        length(CAST(value AS BLOB)) BETWEEN 1 AND 128
        AND instr(CAST(value AS BLOB), x'00') = 0),
    first_seen INTEGER NOT NULL CHECK(first_seen >= 0),
    last_seen INTEGER NOT NULL CHECK(last_seen >= first_seen),
    PRIMARY KEY(account_id, value)
) STRICT;

CREATE TABLE visits (
    visit_id TEXT PRIMARY KEY NOT NULL CHECK(
        length(CAST(visit_id AS BLOB)) BETWEEN 1 AND 64
        AND instr(CAST(visit_id AS BLOB), x'00') = 0
        AND visit_id NOT GLOB '*[^A-Za-z0-9_.-]*'
        AND visit_id NOT IN ('.', '..')),
    account_id INTEGER NOT NULL REFERENCES accounts(id) ON DELETE RESTRICT,
    alias TEXT NOT NULL CHECK(
        length(CAST(alias AS BLOB)) BETWEEN 1 AND 128
        AND instr(CAST(alias AS BLOB), x'00') = 0),
    occurred_at INTEGER NOT NULL CHECK(occurred_at >= 0)
) STRICT;

CREATE TABLE roles (
    account_id INTEGER PRIMARY KEY NOT NULL
        REFERENCES accounts(id) ON DELETE RESTRICT,
    role TEXT NOT NULL CHECK(role IN ('owner', 'moderator', 'analyst'))
) STRICT;

CREATE TABLE bans (
    account_id INTEGER PRIMARY KEY NOT NULL
        REFERENCES accounts(id) ON DELETE RESTRICT,
    starts_at INTEGER NOT NULL CHECK(starts_at >= 0),
    expires_at INTEGER CHECK(expires_at IS NULL OR expires_at > starts_at),
    reason TEXT NOT NULL CHECK(
        length(CAST(reason AS BLOB)) <= 256
        AND instr(CAST(reason AS BLOB), x'00') = 0)
) STRICT;
```

La validación UTF-8/controles sucede además en C++; no se introducen funciones SQL personalizadas. Las seis sentencias constantes del código son las anteriores, sin el punto y coma final, tal como se persisten en `sqlite_schema.sql`. Se compara ese texto con cada sentencia esperada, preservando literales y constraints, sin ejecutar SQL persistido ni aceptar variantes arbitrarias como equivalentes. El contrato permite rechazar una DB cuyo DDL haya sido reformateado externamente aunque su comportamiento aparente sea equivalente.

La comparación incluye conjunto exacto de tablas y columnas, ordinales, tipos, NOT NULL, PK, defaults ausentes y STRICT mediante metadata del motor (`table_list`/`table_xinfo`); conjunto y columnas de índices UNIQUE/PK mediante `index_list`/`index_xinfo`; y FK a accounts(id), sin cascadas, con DELETE RESTRICT y UPDATE NO ACTION. El texto CREATE exacto cubre los CHECK, además de esas comprobaciones estructurales. Sólo se aceptan los tres índices automáticos que derivan de UNIQUE accounts(steam_id64), PK aliases(account_id,value) y PK visits(visit_id), con origin y columnas correspondientes, sql NULL y nombres generados previstos. No se toleran índices adicionales, tablas internas arbitrarias, triggers o views. `foreign_key_check` debe devolver cero filas. Metadatos deben contener exactamente la fila singleton=1 y el server_id solicitado.

Versión 0 sólo admite application_id=0 y **cero filas en sqlite_schema**, contando también nombres sqlite_ e índices automáticos; no se filtran para adoptar una DB preexistente. Una DB vacía sin header materializado también es inicializable si el motor la reconoce como vacía. Se inicia BEGIN IMMEDIATE, se vuelven a leer versión/appid/esquema dentro de la transacción, se crean las seis tablas, se inserta el server_id con bind y se establecen appid/user_version; sólo un COMMIT exitoso publica la inicialización. Cualquier error provoca rollback. Versión futura/foreign appid, esquema incompleto o server_id diferente fallan sin reparación ni cambios. Version1 requiere appid exacto, estructura exacta y metadatos; nunca se ejecutan triggers/views inesperados para comprobarlos.

La conexión configura y comprueba FK habilitadas, DEFENSIVE ON y TRUSTED_SCHEMA OFF; establece busy timeout de 250 ms y exige soporte OMIT_LOAD_EXTENSION. Usa FULLMUTEX y NOFOLLOW al abrir; un mutex privado de Impl protege cada operación pública completa, incluyendo la transacción y el cache. Los helpers no vuelven a tomar ese mutex. `is_banned` consulta mediante `ban` y evalúa el intervalo sin tomar otro lock. Move/destrucción requieren que los callers hayan dejado de usar la instancia. El timeout limita la espera de busy de SQLite, no promete un deadline duro de I/O.

Los controles de **esquema** comprueban DDL/columnas/índices/FK/STRICT al abrir y al cambiar `PRAGMA main.schema_version` dentro de una transacción. Sólo entonces se ejecuta foreign_key_check completo. Con cookie igual se consultan cookie/version/appid y la fila singleton/server_id, sin recorrer el historial. Este cache es una optimización de una DB local confiable: no protege frente al owner que manipule bytes o el cookie. Lecturas usan BEGIN; escrituras BEGIN IMMEDIATE; rollback RAII se mantiene activo hasta COMMIT exitoso. SQL y DDL son constantes; valores y LIMIT van como binds.

Los controles de **datos devueltos** son independientes del cache: cada fila leída comprueba tipos SQLite, tamaños antes de copiar texto, formato/rango Steam, ID positivo, timestamps, valores Role y UTF-8/controles de alias/reason. Las listas limitan filas devueltas a 1..256 y verifican ese límite también al construir el resultado. El esquema por sí solo no garantiza UTF-8/control válido en una fila escrita externamente; esos datos se rechazan al leerlos.

## Pruebas preparadas

25 casos de prueba propios, además de subcasos y bucles: los 22 casos RED se conservan y cubren inicialización/texto uint64, reopen, aislamiento entre dos DB, replay/conflictos y atomicidad de alias, tiempos fuera de orden/lecturas limitadas, cuenta inexistente, permisos explícitos/revocación, fronteras de ban, validación IDs/UTF-8/controles/timestamps, SQL literal parametrizado y redacción de errores. Fixtures SQLite reales cubren versión futura, appid extranjero, otro servidor, schema0 no vacío (tabla o view), tabla faltante/constraints alterados, views/triggers inesperados sin ejecución y bloqueo de inicialización.

Los tres casos añadidos antes de GREEN cubren: dos hilos usando el mismo LocalStore (get_or_create del mismo vínculo y 64 visitas distintas, captura de excepciones sin asserts dentro de hilos, verificación de alias/filas/replays tras join); un trigger añadido después de abrir que debe rechazarse sin ejecutarlo; y filas con UTF-8/controles inválidos que deben rechazarse aunque el esquema no cambie. Estos casos todavía no tienen resultado ejecutado.

RED externo: `implementation/evidence/m3-cpp-red/gate-summary.json` y `suite-results.json` registran configure=0, build=0 y CTest=8; storage 2/56 casos PASS, 54 FAIL contra stubs, 0 skips; foundation 90/90 y master 10/10 PASS. Fuente de evidencia: dueño de validación externo, revisada por el padre; se leyeron sus resúmenes, no se ejecutaron esos comandos por este escritor. Estos números pertenecen al conjunto RED integrado anterior, no a las tres pruebas nuevas ni a esta implementación. GREEN sigue pendiente.

Auto-revisión de fuente: propiedad/lifetime de statements y conexión RAII; transacciones con rollback ante excepciones; mutex por operación; nada de SQL concatenado con datos; esquema cerrado y sin ejecución de DDL recuperado; bind/UTF-8/rangos y filas acotadas; errores propios sin datos; roles explícitos y scopes lógicos. El ensayo busy comprueba ausencia de migración bajo bloqueo y retry tras rollback del fixture, sin inyectar fallo a mitad de cada DDL. Los conflictos comprueban efectos atómicos observables sin asumir orden interno.

Límites abiertos: no autenticación Steam ni permisos del panel; no protección ante un owner con acceso al archivo o clones con igual server_id; NOFOLLOW de SQLite no se presenta como eliminación de TOCTOU ni control de todos los ancestros; no se mide rendimiento de DB grande ni se garantiza deadline de I/O. El scan FK completo ocurre al abrir/cambiar esquema y puede crecer con el contenido; las lecturas ordenadas aún pueden recorrer datos aunque el número de resultados sea limitado. No hay hooks productivos ni fixtures de Steam real. Este módulo no implementa economía, sesiones, autenticación ni endpoints.
