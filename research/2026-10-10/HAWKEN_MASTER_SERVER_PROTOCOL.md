# Hawken: descubrimiento, matchmaking y supuesto «master server»

Cierre estático original:2026-10-10, sobre `F:\SteamLibrary\steamapps\common\Hawken`, sin ejecutar el juego/contactar infraestructura externa en esa fase. Actualización2026-10-11: existen Master y backend C++ propios ensayados localmente; el flujo completo del cliente original sigue pendiente.

[Ensayo094223](../../implementation/evidence/local-steam-routing-20261011-094223/parent-runtime-review.json), SHA-256 `3a5ab95bad6718a3ab05a98d52e1fb8867bb47c1c6c4b41e3df6e2b738f6ca8d`: Master propio `0d801561c2f85c9d4d0f79bf4f38edb636b6fba65b6661263662f3631cfa1acc` escuchó127.0.0.1:18080, listado200; esa única consulta fue del operador. Backend separado80 atendió Steam/perfil y cinco GET sync200 del EXE original `4a861ecb4ac7add9554f8daf8f98ad2c12b9cb6c1865a83e8be808beb46ea8a2`. **CONFIRMADO** procesos separados/HTTP local; **NO VERIFICADO** uso del Master por Hawken, anuncio de GameServer real, reserva/sesión/partida. El Master conserva sólo directorio; no almacena cuentas ni saldos.

Requisito del propietario para una futura reconstrucción: **Steam legítimo obligatorio**. Los caminos alternativos observados se documentan como evidencia histórica del cliente; no autorizan una vía de acceso sin Steam ni sustituyen la validación de identidad exigida. Esta investigación estática no verificó autenticación/admisión en ejecución.

## Arquitectura acordada y evidencia histórica

**Separación de latencia confirmada estáticamente:** PortTester usa directamente la IPv4 de EndPoint por TCP15520, con puerto fijo independiente de listing.Port; no consulta al Master para medir ese trayecto. Original EXE SHA-256 `4a861ecb4ac7add9554f8daf8f98ad2c12b9cb6c1865a83e8be808beb46ea8a2`, RVA `0xa4fbb0` y `0xa6bc10`. [Evidencia y límites](../../implementation/evidence/native-port-tester/contract.json). Nexus entrega el destino; la compatibilidad futura de Core requiere considerar ese puerto separado del UDP de partida. No se ha implementado el listener ni probado que el navegador muestre el resultado; una conexión TCP exitosa tampoco demuestra disponibilidad del mundo.

La arquitectura conserva un **MasterServer que sólo lista servidores por región**. Auth, perfiles, SQL e interfaces de sesión son responsabilidades separadas. El **Server Creator** será una descarga con dependencias y panel de mapas, eventos y XP; no habrá launcher. INI-only está **contradicho por la inicialización estática**: el SDK lee el archivo, pero el EXE sobrescribe `meteoruri`. La instrucción posterior autoriza DLL propia+TOML sin cambiar originales; carga, URI y socket locales están comprobados según [HAWKEN_AUTH_PROTOCOL.md](HAWKEN_AUTH_PROTOCOL.md). Esto no demuestra cambio de backend por servidor desde el browser nativo ni failover entre Masters. El diseño propio de publicación/leases tampoco se atribuye al servicio histórico.

Los flujos Meteor de matchmaking, anuncios, reservas y sesión que se documentan abajo son evidencia histórica y conservan sus contratos recuperados. El uso descriptivo de «master» para agruparlos no asigna esas funciones al MasterServer futuro.

## Resultado

**CONFIRMADO:** la configuración declara `OnlineSubsystemMeteor` y `bUseLegacyMatchmaking=true`; los paquetes contienen interfaces concretas para buscar listados, crear solicitudes de matchmaking, reservar plazas y recibir un destino de partida. El SDK contiene los contratos cliente REST de listados, anuncios y sesión, con verbos, filtros y parsers JSON recuperados. El ejecutable contiene constructores de tareas de matchmaking y referencias a esos caminos.

**PROBABLE:** la función de un master —descubrir partidas, asignar un servidor y mantener anuncios/reservas— reside en servicios Meteor/Storm consumidos por `OnlineSubsystemMeteor`, con un beacon adicional para la reserva de partido. Esto no demuestra un proceso «master» independiente ni un protocolo único que pueda implementarse aparte de autenticación y los demás servicios.

**NO VERIFICADO:** el protocolo de registro, actualización y heartbeat del lado servidor; la selección de ramas legacy y la secuencia completa hasta la admisión UE3; el valor efectivo del override de URI, failover y múltiples masters. Ya se recuperaron ramas concretas de búsqueda, polling, cancelación, viaje y argumento de override del EXE. Los contratos cliente estáticos recuperados no equivalen a observar la aceptación del servicio.

Las interfaces y campos de objetos no son por sí mismos mensajes de red. No se propone un servidor compatible sobre la base de esos nombres.

## Identidad y trazabilidad

Los paquetes de esta sección están en `F:\SteamLibrary\steamapps\common\Hawken\HawkenGame\CookedPCConsole`. Las referencias `virtual` son posiciones en la reconstrucción descomprimida de análisis, no offsets del archivo original. El JSON de cada paquete conserva `zlib_blocks`, con offset original, longitud comprimida, rango virtual y SHA-256 de cada bloque. Esta distinción permite volver al bloque original que contiene un objeto sin inventar una correspondencia byte a byte.

| Artefacto original | SHA-256 |
|---|---|
| `Binaries\Win64\HawkenGame-Win64-Shipping.exe` | `4a861ecb4ac7add9554f8daf8f98ad2c12b9cb6c1865a83e8be808beb46ea8a2` |
| `OnlineSubsystemMeteor.upk` | `bab9ad82bd5fae5afe30707513caa27605c7ece8f296461678ae07cd759ed310` |
| `IpDrv.upk` | `432087362da101de1189e88a260e4a18c016320b8b9ccd9911df59eebe04b4ef` |
| `HawkenGame.upk` | `d1a33a064de4451e5a39e13a28e4a8f2913ce6d9b54ca65bae47118d32849ed0` |
| `HawkenGame\Config\DefaultEngine.ini` | `6f839d73d16f512fbbf29e4d35ea12ba1e8604c8372ea72cad90dabff2526599` |
| `Engine\Config\BaseEngine.ini` | `b550c921f85f41aa947e4502990e6f381163e82836ef64141aaff05cb5f222d9` |

Evidencia reutilizable:

- `evidence/game/config-evidence.json`: configuración original, hash, sección, línea y offset de archivo.
- `evidence/game/exe-network-strings.json`: cadenas ASCII/UTF-16LE con offset original y RVA.
- `evidence/game/exe-network-lea-candidates.json`: candidatos de referencias LEA con rango de función de `.pdata`. Un candidato necesita desensamblado antes de afirmar una referencia de instrucciones.
- `evidence/game/matchmaking-client-disassembly.txt`: desensamblado que verifica los tres constructores de tareas descritos abajo.
- `evidence/game/OnlineSubsystemMeteor.upk.json` y `.details.json`: tablas de nombres/imports/exports, relaciones Outer, defaults tipados y enums.
- `evidence/game/IpDrv.upk.json` y `.details.json`: declaraciones del beacon, estados y resultados de reserva.

El registro REA del EXE preparado por el investigador principal es `evidence/rea-artifact-main.json`, Evidence ID `ev_27a70b00609b60b18ceca80c9918417751cb7c28befa2575a45dbed3cc2ad637`. El lote Ghidra de 29 funciones terminó sin fallos: overview Evidence ID `ev_732ca8087b47fc6de7ef402ffa3dc06845d44dd7265328dbd04fde2eb07fbadf`, batch `ev_f40e06077c59faa017ce58b417dd5336de0da3db2a4af0895c3744c66a44651c`. `evidence/ghidra-main-functions.jsonl` normaliza RVA, entry, SHA-256, pseudocódigo, ID y fuente; conserva los originales en `evidence/rea-main-run2/network-functions.json` y `evidence/rea-main-complete-bundle.json`. Un segundo proyecto privado persistente produjo `evidence/ghidra-main-helpers.jsonl`: seis helpers completos, SHA original y exit 0. Su autoanálisis se limitó a 180 s y quedó incompleto; el log confirma desactivados PDB Universal y Decompiler Switch Analysis, sin confirmar omisión de Address Tables. Ese límite afecta cobertura de referencias/ausencias, no permite elevar un xref negativo a prueba de ausencia en todo el programa. Los símbolos `FUN_...` y campos sin tipo son salidas del decompilador; un nombre inferido no reemplaza la comprobación de código. Los scripts locales son evidencia independiente de REA.

## Interior de UPK descompilado: región, búsqueda y registro

Se inspeccionó bytecode con **UELib 1.13.0 / UE Explorer 1.6.2**, `BuildTarget.Hawken`. Las copias derivadas normalizan sólo el header comprimido: eliminan los descriptores zlib, reubican su tail y fijan flags/count de compresión a cero; conservan el tag custom y todos los offsets/payload desde el primer offset virtual. El lector suprime la comprobación de firma custom. [upk-normalized-copies.json](evidence/upk-normalized-copies.json) conserva SHA original, derivado y rangos. No se modificaron los paquetes originales.

Los manifests de `Engine`, `IpDrv`, `OnlineSubsystemMeteor` y `HawkenGame` terminaron exit 0, pero eso no prueba recuperación integral: `Engine` registró errores de PropertyTag/defaults. Se revisaron **funciones específicas**, sin atribuir validez general a defaults. [upk-reviewed-functions.json](evidence/game/upk-reviewed-functions.json) cruza fullpath, export positivo original, offset virtual, bloque comprimido, flags/ScriptSize y SHA del `.uc`. El ordinal del lector no es índice export; imports que producen `function ();` no son cuerpos recuperados. [upk-native-token-map.json](evidence/game/upk-native-token-map.json) recupera las firmas de operadores de Core que el decompilador presenta como `__NFUN_n__`; no recupera sus implementaciones nativas.

### La región del navegador procede de datos de items

**CONFIRMADO estático, HawkenGame.upk (SHA de la tabla de identidad):**

| Cadena de funciones | Export / offset virtual | Hecho observado |
|---|---|---|
| `R_GFx_MechMenu_Base.InitializeGameItemTypeCollectionForUI` | `21194 / 0x9142e2` | Limpia `ServerRegionItems`, itera la colección y lee `invType`, `Disabled`, TypeId; entrega el item a `AddGameItemTypeForUI`. |
| `AddGameItemTypeForUI` → `InitializeServerRegionForUI` | `21254 / 0x9161a2` → `21584 / 0x9237a6` | El case **55** agrega el item a `ServerRegionItems`. No es un listado fijo de países. |
| `R_GFx_MainMenu.AddDynamicServerRegionsToBrowser` | `25899 / 0x9ac96f` | Lee los atributos de la tabla siguiente y crea objetos UI con `regionName`; genera listas versus y competitiva. |
| `SetDefaultRegion` | `25902 / 0x9ad188` | Consulta `GetUnrankedMatchRegion(false/true)`, busca el objeto correspondiente y aplica fallbacks UI. |
| `HandleRefreshServerList` | `26513 / 0x9be683` | Obtiene `RegionObj.GetString("regionName")`, llama `SetUnrankedMatchRegion(RegionName)` y `RefreshServerList(0)`. |
| `RefreshServerList` → `SubmitServerListQuery` | `26568 / 0x9bfbc6` → `26570 / 0x9bff19` | Configura search, instala delegado y ejecuta `SearchDataStore.SubmitGameSearch(playerId,false)`. `SetupJoinGame` (`26219 / 0x9b6f72`) usa `UDKDataStore_GameSearchBase`. |

| Atributo exacto | Valor/fallback usado por el script |
|---|---|
| `Disabled` | `"0"`; omite `"1"` |
| `RegionName` | Debe ser no vacío para esa entrada |
| `RegionLocId` | `"-1"`; un índice mayor que −1 consulta `DynamicServerRegionNames` |
| `RegionFallbackName` | Nombre de presentación alternativo; después usa el texto World y el índice |
| `IsVersusRegion` | `"1"`; habilita entrada versus con `regionName=RegionName` |
| `IsCompetitiveRegion` | `"0"`; habilita entrada competitiva con `regionName=CompRegionNamePrefix + RegionName` |

`InitServerBrowserRegions` (`25877 / 0x9ac21c`) crea entradas especiales SelectRegion/AllRegions según ChinaBuild; no demuestra las regiones activas de un servicio. `OnReadGameItemTypeCollectionComplete` (`26107 / 0x9b34a1`) enlaza éxito de lectura de items, inicialización de UI y construcción dinámica. Las claves son accesos locales a atributos; no constituyen por sí mismas un esquema REST nuevo ni una lista completa de regiones disponibles hoy.

**Filtro local demostrado:** `R_GFx_MainMenu.OnFindOnlineGameServersCompleted` (`26634 / 0x9c114f`, flags `0x20002`) lee `GetUnrankedMatchRegion()`, itera `MeteorOSS.OnlineGameServers` y aplica comparación de región al array recibido. Admite excepciones World, AllRegions y presencia del texto AllRegions dentro de la región del listing; cuando la región es USA, deriva USW/USE buscando `usw` en el nombre del servidor. `AlwaysShowInRegion` interviene en el filtro de servidores vacíos, no elimina automáticamente la comprobación de igualdad regional. El cuerpo exige MaxUsers>0 y nombre no vacío; filtra tipos versus/cooperativo y entradas FTUX. Para Roles no vacío lo divide por coma, llama HasRole por entrada y descarta si alguna no cumple; para autorizado remapea la región a Custom. Estos controles son **de presentación local**, no pruebas de admisión ni de permiso backend.

El callback alternativo `OnFindOnlineGamesComplete` (`26587 / 0x9c05c7`) itera resultados de SearchDataStore/GameSettings, arma `IP=ServerEndPoint + ":" + ServerPort` y agrega objetos UI; **ese cuerpo no contiene el filtro regional anterior**. `MainMenu.Start` (`25847 / 0x9ab392`) registra el delegado de OnlineGameServers. No se equiparan las dos rutas ni se da por probada selección legacy efectiva sólo por la configuración `bUseLegacyMatchmaking=true`. Devolver un subconjunto regional en el nuevo MasterServer requiere mantener consistente el contrato de listado y la ruta seleccionada; el script original puede filtrar localmente una respuesta más amplia.

### Bindings nativos y alcance del registro

[upk-native-bindings.json](evidence/game/upk-native-bindings.json) conserva los pares de punteros nombre/función del EXE original, offsets/RVA y bytes; [ghidra-main-upk-native-bindings.jsonl](evidence/ghidra-main-upk-native-bindings.jsonl), [ghidra-main-upk-native-consumers.jsonl](evidence/ghidra-main-upk-native-consumers.jsonl) y [ghidra-main-upk-virtual-implementations.jsonl](evidence/ghidra-main-upk-virtual-implementations.jsonl) conservan cuerpos completos con SHA original. Todos los offsets EXE siguientes son RVA; el offset original de `.text` es RVA menos `0xc00`.

**CONFIRMADO:** `InitializeServerListing` es declaración nativa privada/final del paquete (`257 / 0x59af1`, flags `0x40401`). Su binding `0x400560` llama `0x7fdf60`, cuyo cuerpo **sólo retorna 1**; evidencia adicional [ghidra-main-upk-auth-helpers.jsonl](evidence/ghidra-main-upk-auth-helpers.jsonl). No publica un listado mediante red en ese cuerpo. Esto no se generaliza a `CreateOnlineGame` u otros métodos.

`RefreshServerListing` (`1902 / 0x7265e`, flags `0x20400`) enlaza wrapper `0xb76280`, virtual `+0x698` de la tabla `0x169c610`, implementación `0xbdc880`. Ésta busca dentro del array cache `subsystem+0x930`, count `+0x938`, stride `0xc4`, compara strings en entrada `+0x10` y devuelve una copia de la entrada o struct vacío. **No realiza HTTP en ese cuerpo**. `FindOnlineGameServers` (`1087 / 0x670d0`) enlaza `0xb6fcd0` → virtual `+0x478` → `0xbc0380`, que crea una tarea con `0xb7a560` y la encola con `0xbab6d0`; la consulta REST concreta requiere seguir esa tarea. Los punteros exactos están en [upk-native-vtable-candidates.json](evidence/game/upk-native-vtable-candidates.json); el nombre del archivo refleja la fase de extracción, no valida por sí solo la clase.

`GetServerRegion` (`2050 / 0x76680`) enlaza `0xb69e10` → tabla `0x169d500`, slot `+0x370` → `0xb8bbc0`. Su orden estático es: intento de argumento **`ServerRegion=`**, configuración **`[OnlineSubsystemMeteor.OnlineSubsystemMeteor] Region`**, luego **`[URL] ServerRegion`**, finalmente string **`255`**. [upk-native-consumer-literals.json](evidence/game/upk-native-consumer-literals.json) conserva sus bytes originales; la función prueba el orden de llamadas y fallbacks, sin probar qué archivo/config efectiva se cargó. Este getter de región de host es distinto del selector de región de navegador.

`R_MPGameBase.RegisterServer` (`9622 / 0x7d3ed8`) llama a `super(GameInfo).RegisterServer()` sólo con `NetMode==NM_DedicatedServer`. El cuerpo stock `Engine.GameInfo.RegisterServer` (`14947 / 0x411ec8`, SHA Engine `8fff59b404ea4f9dd41330da618432fe50e4a26ce7487379efeb76ce0c4e859c`) crea settings, `UpdateFromURL`, instala delegado y llama `CreateOnlineGame(0, SessionName, settings)`. Su rama LAN puede limpiar delegates de auth; se conserva como evidencia histórica y **no sustituye Steam obligatorio**. La presencia de esa rama no prueba que la instalación incluya un servidor dedicado utilizable.

La declaración Meteor `CreateOnlineGame` es nativa (`2064 / 0x76924`) y usa `0xa44d80` → tabla `0x169d500`, slot `+0x288` → `0xb84690`. Ésta administra settings/callbacks y deriva la rama no LAN a slot `+0x328`, target `0xb84060`. **Su cuerpo recuperado sólo retorna `0x80004005`**, un resultado de fallo; no publica servidor. `FindOnlineGames`, wrapper `0xa44ef0`, termina en `0xa5a860` y deriva rama no LAN a slot `+0x308`, target `0xb870b0`: valida objetos, crea tarea con tabla `0x16a1ad0` y la encola, retornando `0x3e5` (estado pendiente en ese flujo). `UpdateOnlineGame` Meteor (`2069 / 0x76a18`) usa `0xb6a070` → slot `+0x388` → `0xb9aba0`, que actualiza estado local/settings y devuelve 1; no hay publicación HTTP en ese cuerpo. En cambio el método del paquete IpDrv (`2761 / 0x93369`, flags `0x24000`) es un stub de script vacío; no debe confundirse con el override Meteor. Evidencia final: [ghidra-main-upk-listing-producers.jsonl](evidence/ghidra-main-upk-listing-producers.jsonl). Se demuestra esta ruta concreta, no ausencia universal de un productor servidor en todos los artefactos.

La asociación de la tabla GameInterface se trazó desde registro `0xb8a050`, callback constructor `0xb8ede0` → `0xb7ae30`, que instala `0x169d500`. La tabla EntryPC se instala en callback `0xd934c0` del registro `0xd75910`: `SetUnrankedMatchRegion` wrapper `0xd27260` → `0xdb9010` → slot `+0xb88` target `0xdd3f30`; éste modifica strings de perfil `+0x21c`/`+0x22c`, construye claves **`UIFlags.DefaultMatchRegion` / `UIFlags.DefaultCoopRegion`** y entrega el objeto de settings al subsystem virtual `+0x498`. `GetUnrankedMatchRegion` wrapper `0xd273e0` → slot `+0xcc0` target `0xd81fe0` lee esas dos strings según selector. El decompilador pierde un parámetro del wrapper getter: no se toma su constante cero aparente como prueba de que el selector se ignore. [upk-native-constructor-disassembly.txt](evidence/game/upk-native-constructor-disassembly.txt) conserva instrucciones de registro/constructores. La persistencia HTTP de settings pertenece al informe auth, no al directorio regional.

La tarea de búsqueda tiene constructor `0xb7a560` y método en tabla `0x16a1ad0`, slot `+0x20` → **`0xbb9cf0`**, recuperado en [ghidra-main-upk-http-producers.jsonl](evidence/ghidra-main-upk-http-producers.jsonl). Éste construye `MeteorSdk::GameServerListing`, fija **GameVersion**, **Map** (tarea `+0x256c`) y **GameType** (`+0x257c`), usando imports IAT `0x128ef58`, `0x128ef78`, `0x128ef88`; luego llama al consumer del subsystem `+0x270`, virtual `+0x08`, con ese filtro. La tarea creada directamente por FindOnlineGames deja Map/GameType vacíos; FindOnlineGameServers copia sus argumentos. **El cuerpo examinado no llama SetRegion**: una lectura de configuración y diagnostic de región no reemplaza una asignación de filtro. La capacidad SDK de GET con filtro `Region` y el filtro local del callback UI están confirmados por separado. [pe-linkage.json](evidence/pe-linkage.json) identifica los imports; [upk-task-method-disassembly.txt](evidence/game/upk-task-method-disassembly.txt) conserva las llamadas verificadas.

La identificación del consumer de listados enlaza la consulta hasta REST: core SDK `+0x60` → `0x1f990`, interfaz secundaria GameManagerConsumer (tabla `0x1a1d18`, this offset `0x20`), slot `+0x08` → thunk `0xbd70` → **`0xb7a0`** → **`0x7f0d0`**, cuerpo SDK del **GET `/gameServerListings`** con filtros opcionales Region/GameVersion/Map/GameType/ServerName ya documentado abajo. [core-listing-event-getters.json](evidence/auth/core-listing-event-getters.json), [ghidra-sdk-upk-linked-consumers.jsonl](evidence/ghidra-sdk-upk-linked-consumers.jsonl) y [ghidra-sdk-services.jsonl](evidence/ghidra-sdk-services.jsonl) conservan punteros, RTTI, thunks y cuerpos; DLL SHA `f63fef02ffb517ca7844a769d03412ad82bd22198397302ee3c090c63a970c57`. Esto cierra **productor cliente de consulta**, no publicación/heartbeat del servidor ni actividad de un servicio remoto.

El callback de tarea **`0xb99f40`**, offset original `0xb99340`, está recuperado en [ghidra-main-upk-query-callback.jsonl](evidence/ghidra-main-upk-query-callback.jsonl), mismo SHA del EXE. Cuando `task+8` no es nulo, consulta virtual `+8`, guarda `resultado == 0` en `task+0x24`, construye un objeto mediante `0x734d20` y lo entrega con `0xa7fed0`, usando además virtual `+0x10`. **CONFIRMADO:** empaquetado y entrega local del resultado. Este cuerpo no identifica por sí solo cuál de los dos callbacks de la interfaz recibe la consulta ni serializa un filtro regional.

La dependencia stock de búsqueda también está recuperada: `UDKBase.UDKDataStore_GameSearchBase.SubmitGameSearch` (**export 1050 / offset virtual `0x63f2f`**) registra `OnSearchComplete`, llama al método super y quita el delegado si éste falla; `OnSearchComplete` (**1052 / `0x64016`**) lo retira cuando no quedan consultas y llama super. Identidad original SHA `4e2e6caf4141f8f01d6d1f45c9bcb570991b63c37b3d89eb1e0ba79f55d14064`, cuerpos y bloques comprimidos originales en [upk-additional-reviewed-functions.json](evidence/game/upk-additional-reviewed-functions.json). Es apoyo estático de la ruta de data store, sin demostrar ejecución ni reemplazar el filtro regional específico de Hawken.

Las declaraciones native `R_GameSearchDM/TDM/Siege.FilterSearchResult` (HawkenGame exports **24942 / 0x995d2f**, **24954 / 0x996490**, **24949 / 0x995f7d**) comparten wrapper EXE `0x3fc950`, virtual `+0x3e8` → tabla `0x17b7700`, target **`0xd60c00`**. El cuerpo llama a settings virtual `+0x338` con ID `0x800b` cuando hay settings y **retorna 1 sin usar el resultado como predicate**; no contiene filtro de región. [upk-region-filter-bindings.json](evidence/game/upk-region-filter-bindings.json), [upk-region-filter-disassembly.txt](evidence/game/upk-region-filter-disassembly.txt) y [ghidra-main-upk-region-event-final.jsonl](evidence/ghidra-main-upk-region-event-final.jsonl) preservan identidad/binding/cuerpo. El filtro regional comprobado pertenece al callback UI OnlineGameServers descrito arriba, no a estos tres métodos.

## Configuración realmente observada

| Estado | Observación | Ubicación original |
|---|---|---|
| CONFIRMADO | `DefaultOnlineSubsystemName=OnlineSubsystemMeteor.OnlineSubsystemMeteor` | `DefaultEngine.ini`, línea 143, offset `0x103f` |
| CONFIRMADO | `bUseLegacyMatchmaking=true` | mismo archivo, sección `[OnlineSubsystemMeteor.OnlineSubsystemMeteor]`, línea 456, offset `0x43f3` |
| CONFIRMADO | `ClientRequestTimeout=30.0` para el subsystem | línea 457, offset `0x440f` |
| CONFIRMADO | Timeout del manager 300.0 y `ReservedPlayerTimeout=60.0` | líneas 470–471, offsets `0x4629`/`0x4645` |
| NO VERIFICADO | Estos valores se cargaron en una ejecución y qué alternativas los sustituyen | no hay ejecución ni resolución completa de configuración final |

Un selector legacy no prueba qué ramas se ejecutan ni que el conjunto de servicios llamado «Storm» sea otro master. La coexistencia de interfaces nuevas y legacy merece seguir el branch que consume el selector.

## Listados y asignación de partidas

En `OnlineSubsystemMeteor.upk`, la relación Outer vincula estas funciones a la clase `OnlineSubsystemMeteor`; se recuperaron como exports estructurales, no como resultados de búsqueda textual solamente:

| Export | Objeto | Offset virtual del cuerpo | Tamaño |
|---|---|---|---:|
| 257 | `InitializeServerListing` | `0x59af1` | 65 |
| 277 | `Storm_JoinGame` | `0x5a0b6` | 70 |
| 1087 | `FindOnlineGameServers` | `0x670d0` | 75 |
| 1441 | `RequestReservation` | `0x6bf87` | 70 |
| 1452 | `CancelReservationRequest` | `0x6c219` | 70 |
| 1499 | `ConvertStormServerListingToGameSearchResult` | `0x6cdb2` | 75 |
| 1502 | `IsServerListingCreated` | `0x6ce7e` | 65 |
| 1902 | `RefreshServerListing` | `0x7265e` | 70 |

**CONFIRMADO:** estas declaraciones están incluidas en el paquete. No se recuperó un cuerpo de red completo de cada una ni se comprobó su invocación en una sesión.

Las properties hijas y las referencias de tipo de los parámetros concretan la unión entre funciones y listado. `FindOnlineGameServers` declara filtros string `GameType` y `MapName` (exports 1085/1086, cuerpos virtuales `0x67080`/`0x670a8`). `Storm_JoinGame.listing`, `RequestReservation.DesiredServer` y `CancelReservationRequest.DesiredServer` son `StructProperty` que referencian al export 1495 `StormGameServerListing`; cuerpos virtuales `0x5a08a`, `0x6bf5b`, `0x6c1ed`. `RefreshServerListing` declara `currentListing` y `ReturnValue` del mismo tipo, en `0x72632`/`0x72606`. `ConvertStormServerListingToGameSearchResult` recibe ese listado y devuelve una referencia importada `OnlineGameSearchResult` (parámetros `0x6cd86`/`0x6cd2a`). Evidencia: `focused_function_parameters` en el JSON `.details`. Esto confirma la conexión de datos local; no prueba el significado de cada filtro en el servicio ni el formato externo de una consulta.

`StormGameServerListing`, export 1495, cuerpo virtual `0x6cabb` de 623 bytes, define un esquema local de listado. Sus properties exportadas son:

| Tipo de property | Campos observados |
|---|---|
| `StrProperty` | `ListingGuid`, `ServerName`, `MapName`, `GameType`, `VoiceChannelListing`, `EndPoint`, `Region`, `Roles`, `Version`, `Password` |
| `IntProperty` | `UserCount`, `MaxUsers`, `Port`, `ServerRanking`, `ServerScore`, `AveragePilotLevel` |
| `BoolProperty` | `AlwaysShowInRegion`, `RatingUnlocked`, `bIgnoreMMR` |
| `ByteProperty` | `CurrentState` |

Por ejemplo, las properties `EndPoint` y `Port` tienen cuerpos virtuales `0x6c97b`/`0x6c953`; `ListingGuid`, `0x6ca93`. Es un esquema de datos local confirmado. El parser SDK descrito abajo usa keys distintas (`Guid`, `Map`, `Endpoint`) y `Port` string. La adaptación recuperada en `0xbaf3c0` confirma la conversión hacia estos campos locales; los defaults del servicio y su validación siguen **NO VERIFICADOS**. No se deben copiar los nombres locales como keys JSON.

`StormGameServerReservationInfo`, export 230, cuerpo virtual `0x5933c` de 141 bytes, contiene `EndPoint` string, `Port` int y `bSlotReserved` bool. Sus properties están en `0x592ec`, `0x592c4` y `0x59314`. Esto corrobora que el resultado de una reserva puede representar un destino y la concesión de plaza; no demuestra un paquete específico.

La decompilación del EXE RVA `0xbcbfc0` confirma un consumer de eventos de partido XMPP. Comprueba partyId contra el partido actual y compara dataName con `TravelRequestPartyData` sin distinguir mayúsculas. Divide dataValue por el literal `;` (RVA `0x1350c98`, offset original `0x1350098`), recupera cuatro campos posicionales y convierte los últimos dos con `_wtoi`. Construye el listado local con `ListingGuid`, `EndPoint`, `Port` y bandera de reserva, y encola una tarea `GetGameServerListing` por GUID. La cadena del diagnóstico está en original `0x16bdac0`, RVA `0x16be6c0`. **CONFIRMADO:** parsing y dispatch local; falta el formato del transporte XMPP y el productor para establecer escaping, requisitos y validación completos. Este evento no es el paquete binario tipo 6 de PartyBeacon.

## Solicitudes, anuncios y heartbeat

El desensamblado de los rangos siguientes verifica que la referencia de la cadena forma parte de construcción de objetos de tarea: inicializan campos, asignan nombre y vtable, y devuelven el objeto. **No son pruebas de que se haya emitido una solicitud de red.**

| Función identificada por cadena de tarea | RVA de función | Cadena original / RVA |
|---|---|---|
| `MatchmakingClient::CancelRequest` | `0xb7a630`–`0xb7a6d9` | `0x16a1150` / `0x16a1d50` |
| `MatchmakingClient::CreateRequest` | `0xb7a6e0`–`0xb7a79a` | `0x16a1020` / `0x16a1c20` |
| `MatchmakingClient::GetRequest` | `0xb7a7a0`–`0xb7a85f` | `0x16a10c0` / `0x16a1cc0` |

Otras cadenas candidatas incluyen `CreateClientMatchmakingAdvertisement()`, `GetClientMatchmakingAdvertisement()`, `CancelClientMatchmakingAdvertisement()`, `FilterGameServerListings` y `IGameManagerConsumer::GetGameServerListing()`. Son puntos de entrada para seguir hacia las llamadas del SDK; la investigación de endpoints/SDK corresponde al documento de autenticación y servicios del investigador encargado de esos archivos.

Los defaults de clase del paquete aportan valores concretos:

| CDO / property | Valor serializado | Offset virtual de tag |
|---|---:|---|
| `Default__OnlineMatchmakingManager.ClientRequestTimeout` | 300.0 | `0x7a3cf` |
| `.ClientUpdateFrequency` | 2.0 | `0x7a3eb` |
| `.ServerUpdateFrequency` | 2.0 | `0x7a407` |
| `.AdvertisementHeartbeatFrequency` | 10.0 | `0x7a423` |
| `.ReservedPlayerTimeout` | 60.0 | `0x7a45b` |
| `Default__OnlineGameInterfaceMeteor.ServerListingUpdateRate` | 3.0 | `0x76e18` |
| `.PlayersPresenceUpdateRate` | 10.0 | `0x76e34` |

**CONFIRMADO:** los valores y nombres están serializados como `FloatProperty`. **PROBABLE:** sirven de intervalos/timeout en segundos por nomenclatura y configuración concordante. **NO VERIFICADO:** la unidad desde el consumidor, la cadencia efectiva y los mensajes que se envían en cada tick. El heartbeat del anuncio y el heartbeat binario de PartyBeacon son conceptos distintos; no se intercambian automáticamente.

La clase `OnlineMatchmakingManager` contiene `ActiveServerAdvertisements`, `LastAdvertisementHeartbeatTime`, `LastServerAdvertisementsUpdate`, `MMPlayerReservation` y estado `ALLOW_MATCHMAKING`/`DISABLE_MATCHMAKING`. El enum está serializado en `0x79dd6`, miembros ordinales 0/1 y terminador 2. Son estados locales; no se asignan esos ordinales a un protocolo externo.

## Relación con el servidor de partida

La secuencia candidata respaldada por las declaraciones es:

1. Obtener/filtrar listados o crear una solicitud de matchmaking.
2. Obtener un listado/destino (`ListingGuid`, `EndPoint`, `Port`).
3. Solicitar/cancelar reserva y recibir concesión/destino.
4. Viajar al servidor y completar el handshake del motor.

La secuencia global es **PROBABLE**; el lote nativo ya confirma tramos REST → destino → comando de viaje descritos abajo, sin resolver toda reserva/admisión. En el enum local `EJoinServerFrom` de `OnlineSubsystemMeteor`, serial virtual `0x8e013`, existen orígenes `JSF_MATCHMAKING`, `JSF_SERVER_BROWSER`, `JSF_FRIENDS_LIST`, `JSF_GAME_INVITE`, `JSF_DIRECT_CONNECT` con ordinales 0–4. La declaración de `DIRECT_CONNECT` no demuestra que el cliente pueda jugar sin los requisitos de sesión/reserva/autenticación.

El detalle estático del beacon se conserva en [HAWKEN_GAME_SERVER_PROTOCOL.md](HAWKEN_GAME_SERVER_PROTOCOL.md). Su heartbeat de un byte no constituye registro de un servidor en un master.

## Contratos cliente REST corroborados en el SDK

La investigación coordinada del SDK añade evidencia de código a las declaraciones de paquete. Archivo original: `F:\SteamLibrary\steamapps\common\Hawken\Binaries\Win64\Meteor.Sdk.Cpp.Win32.Client64.dll`, SHA-256 `f63fef02ffb517ca7844a769d03412ad82bd22198397302ee3c090c63a970c57`, ImageBase `0x180000000`. Fuentes de decompilación Ghidra 12.1.4: `evidence/ghidra-sdk-services.jsonl`, `evidence/ghidra-sdk-schemas.jsonl` y `evidence/ghidra-sdk-results.jsonl`, con identidad/hash, RVA/VA, rangos, pseudocódigo y referencias por función. `evidence/auth/recovered-contracts.json` integra esos contratos. En estos rangos `.text`, el offset original es RVA menos `0xc00`. El [catálogo de endpoints](HAWKEN_ENDPOINT_CATALOG.md) conserva el detalle y límites del investigador que analizó el SDK; esta sección integra su resultado, sin atribuirlo a ejecución.

| Operación | Constructor / ejecutor RVA | Contrato estático confirmado | Límite |
|---|---|---|---|
| Listing individual | `0x7de00` / `0x7d580` | GET `/gameServerListings/{id}`; ID transformado por `0x6fd50`; parser `GameServerListing::CreateFromJson` en `0x7a610` | campos/tipos abajo; aceptación del servicio no observada |
| Filtrar listings | `0x7f0d0` / `0x7e7e0` | GET `/gameServerListings`; filtros en orden `Region`, `GameVersion`, `Map`, `GameType`, `ServerName`; procesa array de listings | omite strings vacías y aplica escape `0x6fd50` una vez por valor; no se recuperó paginación |
| Servidores de usuario | `0x806b0` / `0x7fee0` | GET `/userGameServers/{userId}`; procesa array de strings como IDs | no son listados completos; omite entradas no-string en el loop observado |
| Crear anuncio cliente | `0x80e80` / `0x4eaa0` | POST `/hawkenClientMatchmakingAdvertisements`; serializer `0x847d0`; `Result` es JSON string copiada a buffer de `0x400` bytes | resultado escalar, no objeto advertisement completo |
| Consultar anuncio cliente | `0x822e0` / `0x81dc0` | GET `/hawkenClientMatchmakingAdvertisements/{id}`; wrapper `0x83fb0` y parser `0x84150` | semántica pública de estados pendiente; campos/tipos abajo |
| Cancelar anuncio cliente | `0x82d40` / `0x82b60` | DELETE `/hawkenClientMatchmakingAdvertisements/{id}`; resultado interno `ApiResult<void>` | no implica cancelación del socket beacon o de una partida ya iniciada |
| Consultar sesión cliente | `0x83b30` / `0x83660` | GET `/session`; wrapper `ClientSession` `0x84da0` y parser `0x84f40` | campos/tipos abajo; vínculo con travel/validación pendiente |

El helper `0x86330` concatena el primer separador `?` (literal RVA `0x1a66d0`) y los posteriores `&` (`0x1a66ec`); `Map=` está en RVA `0x1a66f8`. Sin filtros no agrega query. Evidencia específica: `evidence/auth/helper-0x86330.txt`. El POST de creación usa helper `0x6e900` (xref `0x6e9ca`, call `0x6e9d7`); `0x80b90` es constructor de task, no el ejecutor de esa petición.

El serializer `0x847d0` emite `OwnerGuid`, `Region`, `GameVersion` como strings y `Users` como array de strings. Emite `PartyGuid`, `GameType`, `RequestedServerGuid`, `RequestedMaps` solamente si sus strings no están vacías. **`RequestedMaps` es una string en el código observado**, no un array inferido del nombre. El constructor `0x80e80` valida OwnerGuid, Users no vacío, Region y GameVersion no vacíos antes de enviar. Son condiciones y estructura del consumidor recuperado; no establecen límites máximos ni prueba de aceptación del servicio.

Los parsers inferiores corroboran las siguientes keys, respetando su casing:

| Modelo / parser RVA y offset original | Campos leídos | Condiciones observadas |
|---|---|---|
| `GameServerListing`, `0x7a610` / `0x79a10` | strings `Guid`, `MatchId`, `Region`, `GameVersion`, `ServerName`, `Map`, `GameType`, `Endpoint`, `Port`, `VoiceChannelListing`; convertibles a int `MaxUsers`, `MinUsers`, `ServerScore`, `ServerRanking`, `MatchCompletionPercent`; bool `IsMatchmakingVisible`, `IsPublicVisible`; arrays de entradas string `Users`, `AllowedRoles`; object `DeveloperData` | rechaza raíz null; lecturas condicionales por tipo, sin exigir todos los campos para retornar true. `AllowedRoles` se une con comas en el modelo local. `DeveloperData` enumera keys mediante helper de c-string; valores no-string no se validaron |
| `ClientMatchmakingAdvertisement`, `0x84150` / `0x83550` | strings requeridas `Guid`, `TimeCreated`, `OwnerGuid`, `Region`, `GameVersion`; números `Status`, `SkillRatingAverage`, `SkillRatingSearchMin`; array `Users` con entradas string; strings opcionales `GameType`, `PartyGuid`, `RequestedServerGuid`, `RequestedMaps` | raíz object requerida; si `Status == 1`, exige strings `AssignedServerGuid`, `AssignedServerIp` y número `AssignedServerPort`, convertido a unsigned |
| `ClientSession`, `0x84f40` / `0x84340` | strings `SessionId`, `SessionKey`, `UserGuid`, `ClientAdvertisementGuid`, `AssignedServerGuid`, `AssignedServerIp`; convertible a int `AssignedServerPort` | exige raíz object y todos esos campos para retornar true; no establece longitud, firma ni uso UE3 de `SessionKey` |

En advertisement, el código lee **`SkillRatingSearchMin` dos veces** y almacena resultados en offsets locales `+8` y `+0xc` (xrefs RVA `0x84300`, `0x84326`, `0x8434b`, `0x84371`). Se conserva esa observación sin inventar una key `SkillRatingSearchMax`. La rama `Status == 1` confirma qué campos necesita ese valor; no demuestra por sí sola el significado público de todos los estados.

El envelope común recuperado en RVA `0x708a0` exige raíz object, `Status` no-null convertible a unsigned y `Message` no-null convertible a string, con truncado a `0x3ff` bytes. Serializa `Result` mediante `Json::FastWriter` antes del parser interno; no se observa chequeo explícito de presencia de `Result` y ese parser puede rechazar null. Las keys son **`Status`, `Message`, `Result`**, aunque algunos logs usan minúsculas. Los ejecutores filtran HTTP 200–299; esto no recupera códigos de negocio ni schemas de errores completos. Los headers/auth y demás detalles se conservan en [HAWKEN_AUTH_PROTOCOL.md](HAWKEN_AUTH_PROTOCOL.md).

No se recuperó por esa evidencia un REST de alta/update de `GameServerListing` o heartbeat de servidor. El Client64 SDK puede no contener los consumers del lado servidor; la disponibilidad de endpoints de listado y anuncios de **cliente** no resuelve el protocolo de registro del **servidor**.

## Flujo nativo corroborado del cliente

Todos los RVA siguientes corresponden al EXE y su hash de identidad. Fuente: lote REA/Ghidra `evidence/ghidra-main-functions.jsonl`; offset de código = RVA menos `0xc00`.

| Función RVA | Comportamiento confirmado por llamadas/código | Límite |
|---|---|---|
| `0xbc0380` | `FindOnlineGameServers` crea tarea mediante `0xb7a560` y la encola con `0xbab6d0` | el wrapper por sí solo no demuestra envío |
| `0xbb9cf0` | exige consumers game/auth y comprobación auth virtual; prepara criterio SDK con `GameVersion`, `Map`, `GameType` y llama al filtro | no se declara búsqueda anónima |
| `0xbd0810` | consume array SDK, llama adaptación `0xbaf3c0` y construye resultados de búsqueda UE3; usa `GetEndpoint`, `GetServerPort`, `_wtoi`, `htons`, `GetGuid`, nombre/map/tipo/voz y `MaxUsers - NumMeteorIds(Users)` | conversión de endpoint/puerto confirmada; políticas del servidor no observadas |
| `0xbbd8e0` | construye anuncio con OwnerGuid, Region, GameVersion, RequestedServerGuid; agrega usuario individual o PartyGuid y miembros según rama; llama consumer game manager `+0x20` | estas ramas no prueban modo offline ni registro del servidor |
| `0xbe7a10` | procesa creación, consulta, cancelación y destino del anuncio | estados internos de task y `Status` REST son dominios distintos |
| `0xbeb170` | obtiene listing por GUID; tras éxito consulta sesión si global `DAT_141add394 != 0`; éxito copia SessionId/Key a globals | global inicial 1 en archivo; valor efectivo/overrides y validación posterior pendientes |
| `0xbe44e0` | busca servidores del usuario, obtiene listing, llama adaptador; otras ramas crean anuncio para servidor solicitado, obtienen sesión y ejecutan comando de viaje | grafo completo de ramas y driver final pendientes |

En `0xbe7a10`, el estado interno `task+0x74b0 == 1` espera creación y toma el GUID string devuelto. Si no hay cancelación local inicia GET del anuncio y pasa a estado interno 2; si existe la bandera `task+0x74c8`, inicia DELETE y pasa a 3. En estado 2, cancelación o tiempo transcurrido mayor que el float `subsystem+0x5bc` inicia DELETE; fuera de ese caso espera el resultado y el intervalo local `task+0x1c` antes de consultar otra vez. **`Status` REST 0 mantiene polling; 1 lleva a la rama de asignación**, que recupera AssignedServerIp/Port. El resto de estados/error puede entrar en intentos por otro servidor solicitado. Es conducta estática del cliente, no definición universal de códigos públicos del servicio ni cadencia efectiva observada.

En varias ramas (`0xbe7a10`, `0xbeb170`, `0xbe44e0`) GET de sesión precede a copiar `SessionId` a RVA global `0x1b95300` y `SessionKey` a `0x1b95320`. Por tanto, su conservación por el EXE está **CONFIRMADA**. Se recuperó transformación Base64URL y consumo por un wrapper de paquetes; admisión en gameplay y ticket Steam siguen pendientes.

`evidence/ghidra-session-global-references.json` registra 17 referencias en la base privada limitada. Desensamblado dirigido confirma getters RVA `0xbdf230`/`0xbdf250` que devuelven puntero a SessionId/SessionKey, usando almacenamiento interno o heap según capacidad de string. Esas funciones no transforman las claves. `evidence/ghidra-main-flow.jsonl` recupera tres funciones completas desde la misma base, sin nuevo autoanálisis, con SHA original. Su función `0xa64090` (offset original `0xa63490`) llama ambos getters y pasa `(objeto, SessionId, SessionKey)` a `0xa79510`.

El lote `evidence/ghidra-main-admission.jsonl` contiene otras cuatro funciones completas, mismo SHA y perfil limitado. `0xa79510` (offset original `0xa78910`) inicializa un bloque de 40 bytes; para cada string no vacía crea un array de bytes, llama conversión `0x998120` y copia **8 bytes derivados de SessionId** a `+0`, y **32 bytes derivados de SessionKey** a `+8..+0x27`. `0x9e7000` (offset original `0x9e6400`) escribe estado local `objeto+0x100=1` y llama `0xa64090(objeto+0x98)`. `0xa63c10` (offset original `0xa63010`, lote de cuatro funciones `evidence/ghidra-main-auth-socket.jsonl`) prepara la conexión del driver `+0x80` y, si `DAT_141add394 != 0`, invoca `0x9e7000` sobre ella. **CONFIRMADO:** construcción y almacenamiento de ese bloque en conexión `+0x98`. No es una copia de las strings crudas. `0x998120` normaliza `- → +`, `_ → /` y padding `=` según longitud módulo 4; el algoritmo `0x997d80` combina índices de tabla en sextetos (`evidence/ghidra-main-gameplay.jsonl`, cuatro funciones completas). `evidence/session-native-literals.json` corrobora alfabeto Base64 estándar UTF-16LE RVA `0x1573690` / offset original `0x1572a90`: **CONFIRMADA conversión Base64URL**. También verifica valor inicial 1 de la global gate RVA `0x1add394` / offset original `0x1adc794`, sin probar valor efectivo/overrides. Consumo por el wrapper de paquetes confirmado abajo; vínculo al canal de control y admisión final siguen pendientes; `0xa79510` no muestra comprobación de longitud decodificada antes de sus copias, por lo que los tamaños no prueban longitudes públicas válidas ni formato de ticket Steam. El driver de partida recuperado usa UDP, mientras el PartyBeacon usa TCP: las cadenas de factories están en el informe de partida, no se fusionan estos protocolos.

El consumo posterior está confirmado en `evidence/ghidra-main-session-consumers.jsonl` y `evidence/ghidra-main-session-packet.jsonl` (tres y dos funciones completas): `0x9f6b40` y `0x9e3550` usan gate local conexión `+0x100` para llamar `0xa58380` con contexto `+0x98` antes del envío virtual `+0x2a8`. Construye `SessionId_decoded[8] | contador/nonce[8] | payload_transformado[len] | autenticador[32]`, total `len+48`, pasando la clave decodificada a ambas primitivas. El autenticador de 32 bytes y construcción HMAC se recuperaron en `evidence/ghidra-main-crypto-steam.jsonl` / `evidence/ghidra-main-crypto-primitives.jsonl`; recepción cliente y guards ya recuperados; identificación estructural PROBABLE ChaCha20 original más HMAC-SHA512 truncado256, sin vectores ejecutados ni admisión servidor verificada. El [informe de partida](HAWKEN_GAME_SERVER_PROTOCOL.md) conserva offsets y pasos. Este wrapper no es la FString de Login ni un heartbeat/master protocol.

### Adaptación confirmada SDK → listado local

Helper RVA `0xbaf3c0`, offset original `0xbae7c0`, fuente `evidence/ghidra-main-helpers.jsonl`:

| Datos del SDK | Campo/local offset de salida observado |
|---|---|
| Guid, Name, Map, GameType, VoiceChannelListing | strings en `+0`, `+0x10`, `+0x20`, `+0x30`, `+0x40` (`ListingGuid`, `ServerName`, `MapName`, `GameType`, voz) |
| NumMeteorIds(Users), MaxUsers | enteros `+0x50`, `+0x54` (`UserCount`, `MaxUsers`) |
| Endpoint, ServerPort | string `+0x58` (`EndPoint`); string de puerto convertida `_wtoi` y guardada int `+0x68` (`Port`) |
| ServerRanking, ServerScore | enteros `+0x6c`, `+0x70` |
| Region, AllowedRoles, GameVersion | strings `+0x78`, `+0x88`, `+0x98` |
| DeveloperData | claves recuperadas abajo; no son propiedades superiores del JSON listing |

El helper consume keys de DeveloperData **`AveragePilotLevel`, `RatingUnlocked`, `bIgnoreMMR`, `AlwaysShowInRegion`, `PasswordHash`, `MatchState`**. AveragePilotLevel se convierte por `_wtoi` a `+0x74`; las tres banderas modifican máscaras locales; PasswordHash se copia string a `+0xac` y MatchState pasa `_wtoi` al byte `+0xc0`. Cadenas originales verificadas, con RVA respectivamente `0x16aff18`, `0x16b04d8`, `0x16b0580`, `0x16b0730`, `0x16b0968`, `0x16b0ea0` (offset original = RVA menos `0xc00`); el helper tiene también referencias a segundos literales idénticos de cada key. Esto confirma por qué el struct UE3 contiene campos adicionales al modelo JSON superior. No recupera algoritmo PasswordHash, significado de MatchState, ni prueba que un servidor acepte una contraseña particular.

En el estado local 8 de `0xbe44e0`, llamadas virtuales de la interfaz de juego bajo nombre `Game` producen una cadena de destino, se forma **`open %s`** y una rama agrega **`?Password=%s`**. El comando se pasa al ejecutor del motor. Esto confirma preparación/dispatch de viaje y una opción de password; todavía no proporciona formato completo del destino retornado ni demuestra que SessionKey aparezca en esa URL. El [informe de partida](HAWKEN_GAME_SERVER_PROTOCOL.md) conserva el control-channel y los límites de handshake.

## Registro, descubrimiento y múltiples masters: pendientes explícitos

El ledger `evidence/game/exe-endpoints.json` añade el carrier EXE al catálogo de servicios: conserva nueve URLs literales `v2-services` con variantes staged/main/live y PC/XB1/PS4. El literal PC live `https://v2-services-live-pc.playhawken.com` está en offset original `0x16b0120`, RVA `0x16b0d20`. La decompilación `0xbc3510` confirma `MeteorCore_Create`, asignación inicial de ese URI y sustitución por el string global RVA `0x1d39010` cuando su longitud global `0x1d39018` no es 0/1. Convierte la cadena y la pasa al método inicial del core; obtiene los consumers de auth, game manager, party, voz, XMPP y otros, y crea los managers. También consulta status `services`/`game_client`. `0xc70e20` (offset original `0xc70220`, `evidence/ghidra-main-flow.jsonl`) pasa el argumento de inicialización `param_2`, literal UTF-16LE **`MeteorURI=`** (RVA `0x1710560`, offset original `0x170f960`) y esa global al parser `0x0af610`. **CONFIRMADO:** selección estática default/override y paso del argumento de inicialización, literal `MeteorURI=` y global al helper `0x0af610`; El análisis posterior del informe auth cierra ese helper: **CONFIRMADO** `0xaf610`/`0xaf820` extraen el valor, y `0xc77450` obtiene `GetCommandLineW`, cuya cadena de llamadas alimenta ese parser. [ini-endpoint-native-closure.json](evidence/auth/ini-endpoint-native-closure.json) y [ini-override-source-instructions.json](evidence/auth/ini-override-source-instructions.json) preservan bytes e identidad original. Esa selección histórica por argumento queda excluida del diseño solicitado; la selección sólo por INI exige resolver la sobrescritura observada. **NO VERIFICADO:** valor efectivo en ejecución. Estas funciones no demuestran la selección de la rama `bUseLegacyMatchmaking`. **Variantes de entorno o plataforma no demuestran varios masters, failover ni balanceo**; no se encontró en `0xbc3510` un loop de failover entre los nueve literales.

El ledger distingue categorías candidatas de referencias UE3 stock, URLs auxiliares y direcciones locales, y deja coincidencias no atribuidas como `NO_CLASIFICADO`. Incluye falsas coincidencias posibles de versiones/texto; un dominio o IPv4 encontrado por regex no es automáticamente un endpoint operativo. No se resolvió ni visitó ninguno. El catálogo central ya integra los 106 candidatos del EXE conservando esta distinción y el casing original. En el ledger local, dos URLs se normalizaron: offset `0x177a5c0` corresponde al literal `HTTP://66mm.org/83727` y `0x177a988` a `http://Q19.waps.cn`; ambos permanecen sin atribución operativa.

| Pregunta | Estado | Evidencia / siguiente prueba estática concreta |
|---|---|---|
| ¿Existe registro de servidores? | PARCIAL: camino de host y cuerpos locales CONFIRMADOS, alta remota NO VERIFICADA | InitializeServerListing retorna 1; RefreshServerListing copia cache; CreateOnlineGame no LAN llega a `0xb84060`, que sólo devuelve fallo `0x80004005`; no deducir alta desde nombres |
| ¿Qué publica un servidor? | PARCIAL | esquema local de `StormGameServerListing`; faltan serializer y parámetros de alta/actualización |
| ¿Cómo obtiene el cliente una lista? | PARCIAL: contrato REST y tramo EXE CONFIRMADOS | filtro auth/SDK y conversión a resultados UE3 recuperados; faltan paginación y aceptación del servicio |
| ¿Cómo se asigna partida? | PARCIAL: REST y polling/destino CONFIRMADOS | Status 0/1 y cancelación por timeout recuperados; falta admisión/validación remota; SessionKey ya está vinculada al wrapper de paquetes |
| ¿Hay un master independiente? | NO VERIFICADO | la instalación y los nombres no establecen un rol de proceso desplegable |
| ¿Hay varios masters o failover? | NO VERIFICADO | default/override por línea de comandos ya cerrado en el informe auth; no se recuperó failover entre URI; no inferirlo de servicios/regiones |
| ¿Qué sucede si caduca el anuncio o la reserva? | PARCIAL | `0xbe7a10` inicia DELETE por timeout del cliente; vencimiento del servicio y reserva beacon aún pendientes |

## Administración, mensajes y eventos: evidencia parcial y frontera nueva

El alcance ampliado requiere creador/administrador de servidores, bans, seguimiento de jugadores, comandos de consulta/mensajes personalizados y soporte Linux con SQL autoritativo. Los contratos de listing/anuncio recuperados no proporcionan una API de administración: `AllowedRoles` es un array de strings leído del listing, y `DeveloperData.PasswordHash` se copia al modelo local; **no está demostrada su relación con permisos de administrador, propietario del proceso, ban o comandos**. Tampoco la sesión cliente autoriza administrar otros usuarios o modificar datos persistentes.

El [informe de partida](HAWKEN_GAME_SERVER_PROTOCOL.md) conserva superficies estructurales presentes en HawkenGame.upk y ahora añade cuerpos descompilados, flags RPC/exec y controles concretos de `AdminLogin`, `ServerKickBan`, cambios de mapa, mensajes y XP. Distingue AreAdminCommandsAllowed como gate de intentos, IsServerAdmin como rol local y el gate final de ServerChangeMap. `ServerTeamSay` Hawken llama ServerSay después de su evento, por lo que no se promete aislamiento por equipo. Los eventos XP nativos ya tienen builders recuperados; admisión, permiso completo, serialización de RPC y persistencia autoritativa siguen parciales. Las superficies stock de Engine se distinguen de los overrides Hawken.

Los eventos de matchmaking también están declarados en `HawkenGame.upk` (`evidence/game/HawkenGame.upk.json`, SHA de la tabla inicial), clase `R_EventTracking`: `SendEvent_MatchmakingRequestSent` export **24588**, cuerpo virtual `0x98cd47`; `Cancelled` **24591 / 0x98cf7c**; `Failed` **24594 / 0x98d0ba**; `Fulfilled` **24598 / 0x98d21d**; `PartyDeployed` **24606 / 0x98d456**; `PartyDeployCancelled` **24609 / 0x98d676**. Sus properties hijas incluyen PlayerID, y en Sent/PartyDeployed PartyMemberGuids array, Region/GameType o ServerListingGuid strings según función. Son declaraciones confirmadas, **no nombres de evento HTTP ni schemas públicos deducidos**. Vincularlos a los consumers REST/XMPP y a admisión requiere recuperar bytecode/callers.

Para el plan de reconstrucción, Steam obligatorio y SQL pertenecen a la frontera autoritativa del backend, sin escritura directa del cliente. La evidencia presente no incluye protocolo de comandos administrativos, sincronización de bans globales, persistencia SQL ni servidor Linux distribuido. Esas interfaces nuevas deberán diseñarse y validarse expresamente; no se presentan como contratos existentes en el cliente.

## Límites y aceptación de esta investigación

Se verificó la descompresión zlib de seis paquetes relevantes: cada bloque produjo la longitud declarada, cada chunk terminó exactamente en su rango original, y se recuperaron tablas de nombres/imports/exports y defaults/enums seleccionados sin errores. Los paquetes tienen tag de variante `0xea31928c`, versión 892 y licensee 9. La reconstrucción local conserva bytes de cabecera para trazabilidad; no se declara un `.upk` reutilizable por el juego ni se modificó la instalación.

No se decodificó todo el bytecode UnrealScript ni se estableció todo el grafo de llamadas nativo. No hubo observación de sockets, handshake, tráfico, registro, listas o asignación. Por tanto, este documento entrega un mapa con fuentes y candidatos concretos para continuar la recuperación; **no es una especificación completa ni prueba de compatibilidad de un master emulado**.

`evidence/game/verification.json` registra 45 checks PASS de consistencia estática: hashes de originales y derivados, cantidades/tablas, rangos de exports, referencias de objetos, defaults/enums, enlaces de parámetros y bytes de instrucciones citadas. El PASS corresponde a esa evidencia finita; no representa aceptación de gameplay ni de un servicio emulado.

La ampliación [upk-verification.json](evidence/game/upk-verification.json) comprueba outputs descompilados seleccionados, exports/offsets/SHA originales, bytes de bindings/vtables/literales, cierre de los últimos cuerpos nativos y enlaces/hashes de ambos informes. No convierte outputs stock, defaults parciales o declaraciones native en código de servidor completo ni recupera un alta remota ausente de los cuerpos examinados.

La cobertura exterior a los seis paquetes iniciales consta de 70 paquetes con código. El resumen final [indexed-additional-decompilation-summary.json](evidence/upk/indexed-additional-decompilation-summary.json) coteja 587 exports por índice/offset/tamaño y reutiliza 1.994 outputs stock; los intentos fallidos de selección se preservan como historial invalidado. Las advertencias de PropertyTag/defaults y tablas auxiliares permanecen explícitas: corresponde al informe de partida la revisión puntual de clases/modos/mapas, sin equiparar la cobertura de extracción a recuperación completa de servidores o endpoints.
