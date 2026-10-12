# Hawken: catálogo de endpoints (2026-10-10, actualizado2026-10-11)

**CONFIRMADO estático — medición directa de servidor:** `listing.EndPoint:15520/TCP`. `UPortTester` interpreta EndPoint como IPv4 numérica, ignora el puerto del listing y prueba establecimiento TCP sin payload de aplicación; cinco intentos con timeout1000ms. Archivo `Binaries/Win64/HawkenGame-Win64-Shipping.exe`, SHA-256 `4a861ecb4ac7add9554f8daf8f98ad2c12b9cb6c1865a83e8be808beb46ea8a2`; RVA `0xa4fbb0` (destino fijo), `0xa6bc10` (connect), `0xa54980` (estadísticas), `0xa7ffc0` (consulta). [Contrato](../../implementation/evidence/native-port-tester/contract.json) y [cotejo original](../../implementation/evidence/native-port-tester/parent-review.json). Es cliente→host listado, no cliente→Master ni RTT UDP de combate. Listener propio, asignación a la cifra visual y medición runtime pendientes; el valor1000 también puede resultar de todos los timeouts y no acredita conexión.

El backend personalizado visible en esta muestra es Meteor. El INI distribuido declara `https://v2-services-live-pc.playhawken.com`, y el EXE vuelve a asignar esa base después de leerlo, salvo el override global descrito abajo; el SDK incorpora `https://v2-services-main.playhawken.com`. También contiene PlayFab, DeltaDNA, XMPP, Vivox, Steam y Twitch. Los hallazgos **CONFIRMADOS estáticos** no prueban host resoluble ni servicio activo. Las pruebas ejecutadas se marcan **CONFIRMADO runtime local** y sólo corresponden a nuestros servicios autorizados.

**CONFIRMADO runtime local094223:** el cliente original envió login y tres perfiles200 tras validar Steam; recibió también stat-overflow, achievement-rewards, achievements, game-currency y meteor-currency200, una solicitud por categoría. [Revisión](../../implementation/evidence/local-steam-routing-20261011-094223/parent-runtime-review.json), SHA-256 `3a5ab95bad6718a3ab05a98d52e1fb8867bb47c1c6c4b41e3df6e2b738f6ca8d`; originales EXE/SteamDLL y backend están identificados allí.50requests del juego al backend; la consulta única al Master fue del operador. Game-items404 cinco veces. La captura de ventana fue indisponible tras recuperación; no se atribuye pantalla/error a este ensayo. Menú/sesión/partida **NO VERIFICADOS**. El bloqueo temporal del juego a destinos externos permaneció hasta su cierre.

Los raw paths y formatos de las cinco rutas proceden del [contrato initial-sync](evidence/auth/m4-initial-sync-contract/contract.json), SHA-256 `3cb1850e326b82841e2a5e576dd090e67448b79067ec9d6ed34eef5f430f27bf`. EXE SHA-256 `4a861ecb4ac7add9554f8daf8f98ad2c12b9cb6c1865a83e8be808beb46ea8a2`, RVAs `0xb71630/0xb725c0/0xb715f0`; Meteor SHA-256 `f63fef02ffb517ca7844a769d03412ad82bd22198397302ee3c090c63a970c57`. `/statOverflow/32f2f08c-87f6-45a3-8440-a84242907a95` retorna objeto; `/game-client/achievement-rewards` y `/game-client/achievements/{Region}` arrays; `/gameCurrency/{Guid}` y `/meteorCurrency/{Guid}` enteros signed32. Archivo/hash, producer/parser/consumer y offsets completos están en ese contrato y [auth](HAWKEN_AUTH_PROTOCOL.md). **CONFIRMADO estático**; HTTP200 del cliente original **CONFIRMADO runtime**; parsing/UI final **NO VERIFICADO**.

GET `/session`, sin query/cuerpo/slash final, está cerrado por el [contrato de provisión](evidence/game/session-provision-contract/contract.json), SHA-256 `d3cdcde052e3687982faca96cabce19443a2cce2c777899bebc3e97496ae20d0`. Meteor del hash anterior: enlace `0xbd4c→0xb8d0→0x83b30`, join de URI `0xd98e0`, executor `0x83660`, parser `0x84f40`; EXE `0xbe44e0` consume destino y claves. Result exige siete campos, con puerto numérico signed32; se detallan en [GameServer](HAWKEN_GAME_SERVER_PROTOCOL.md). **CONFIRMADO estático**, no provisión o GET nativo aceptados en runtime. La sesión pertenece a la fachada local del servidor, mientras el Master conserva sólo metadatos regionales.

Actualización2026-10-11: seis GET adicionales ya están en el backend propio. Fuente [menu_sync_service.cpp](../../src/menu_sync_service.cpp), SHA `0e7fc0edc03352ce26760df7d890604898993cfd9f90c309fe0c4058e85d65c4`; evidencia original y RVAs en [remaining-menu-sync-contract](evidence/auth/remaining-menu-sync-contract/contract.json), SHA `32e5118187c10585e2f76be92ea00c21b15c7e306bc1b931173c92c2faa478a2`. [Gate integrado](../../implementation/evidence/menu-reliable-native-full-green/parent-acceptance.json):535/19562. **CONFIRMADO módulo/HTTP propios; NO VERIFICADO consumo nativo de estos seis GET.**

| Método y target exacto | Result inicial propio | HTTP/API | Acceso |
|---|---|---|---|
| GET `/userPublicReadOnlyData/{Guid}` | `{}` | 200/200 | grant y GUID propios |
| GET `/userGameSettings/{Guid}` | `{}` | 200/200 | grant y GUID propios |
| GET `/userStats/{Guid}` | `{}` | 200/200 | grant y GUID propios |
| GET `/userGameItemStats/{Guid}` | `[]` | 200/200 | grant y GUID propios |
| GET `/bundles/` | `[]` | 200/200 | grant vigente |
| GET `/userAchievements/{Guid}?countryCode={regionASCII}` | `[]` | 200/404 | grant y GUID propios |

Estas respuestas representan ausencia local y no progresión histórica. La última conserva HTTP200/API404 porque la rama vacía API200 del cliente intenta una fase siguiente y falla localmente400. URLs codificadas, alias y métodos distintos se rechazan; no se han implementado PUT de settings ni ofertas premium. [Post-sync](evidence/auth/post-sync-menu-contract/contract.json), SHA `4c22c079a4419207e9059772075c804d965a0db3c45470dc3fbac71bf4e08679`, documenta esas operaciones posteriores por separado.

## Cierre de SteamLogin, 2026-10-11

Ampliación **CONFIRMADA estática**: seis GET mínimos del menú, categorías/resultados y ramas de ausencia detallados en [autenticación](HAWKEN_AUTH_PROTOCOL.md) y [contrato de quince gates](evidence/auth/remaining-menu-sync-contract/contract.json), SHA `32e5118187c10585e2f76be92ea00c21b15c7e306bc1b931173c92c2faa478a2`. Cada operación conserva archivo/hash, RVA/offset, executor, parser y callback. Nickname y legacy comparten `/userPublicReadOnlyData/{Guid}`; settings/stats/bundles y unlocked no se deducen del nombre de una clase. Los calendarios son POST PlayFab separados de MeteorURI. Menú y uso nativo de esas nuevas respuestas **NO VERIFICADOS**.

El [ensayo121321](../../implementation/evidence/local-services-starter-20261011-121321/parent-runtime-review.json), SHA `c6fc3b67690628dd1840a3e040ceed81a94b48fffe735e88f85258a8a31a428e`, verificó diez consultas **del operador** al backend/Master ejecutados: catálogo/ofertas/owned sin grant401, métodos inválidos405 y listado vacío200. No hubo cliente en ese ensayo. La implementación protegida del inventario ya pasó su gate de código; la evidencia094223 conserva el404 de su backend anterior.

El [contrato nativo SteamLogin/perfil](evidence/auth/m4-login-wire-contract-native-profile/contract.json), SHA-256 `b573f3f83cfa82e690aa77e116beb2d1c57d86ea6f14e423478e5d9dd8fdd92e`, enlaza la tarea real con POST `/steam/login` y JSON `SteamAuthTicket`. EXE SHA-256 `4a861ecb4ac7add9554f8daf8f98ad2c12b9cb6c1865a83e8be808beb46ea8a2`: `0xbc1720`/offset`0xbc0b20` pide1024bytes, callback163 `0xbdf530`/offset`0xbde930` y encoder `0xbabde0`/offset`0xbab1e0` producen **hexadecimal MAYÚSCULAS**, hasta2048caracteres si se respeta el buffer. `0xbc7df0` entrega el string a Auth+0x38→SDK `0x27c0`→`0x42360`; DLL SHA-256 `f63fef02ffb517ca7844a769d03412ad82bd22198397302ee3c090c63a970c57`. **CONFIRMADO estático**. El getter web separado `0xbc2130`/`0xbb7960` usa Base64URL; no fija el formato del POST SteamLogin. El ensayo local080749 ya confirmó ticket/callback/licencia para una cuenta; el perfil mínimo respondió200. La sincronización completa y la admisión de partida permanecen **NO VERIFICADAS**.

El [contrato de catálogo](evidence/auth/m4-menu-catalog-contract/contract.json), SHA-256 `e493add56a80cbf1972dc92e68393221bf048006f6f278f3696af61ce09f05ed`, cierra dos enlaces de menú **CONFIRMADOS estáticos**. Meteor SHA-256 `f63fef02ffb517ca7844a769d03412ad82bd22198397302ee3c090c63a970c57`: builder `0x62610`/offset`0x61a10` → GET `/game-client/game-items`; builder `0x8baa0`/offset`0x8aea0` → GET `/game-client/game-offers`, ambos sin slash final. EXE del hash citado: wrappers `0xb70bc0/0xb70c00`, tasks `0xbe1d80/0xbe2e80` y conversiones DTO `0xbd0f40/0xbd4690`. Items requiere Result array y atributos string; ofertas admiten array u objeto de objetos, `GameOfferGuid` string y `Fees` objeto con al menos un número. `GameItemsGuid`, Quantity y referencias invType/index deben ser consistentes con el catálogo y assets originales. El cliente reconstruye property sheets desde estos datos antes de ofertas/nickname. Catálogo histórico completo y aceptación de menú **NO VERIFICADOS**. Achievements, wallet y stats conservan enlaces específicos pendientes; un array no vacío no acredita compatibilidad.

**CONFIRMADO estático, ruta construida del nickname:** GET `/userPublicReadOnlyData/{Guid escapado}`, sin slash final y sin query de clave. [Contrato](evidence/auth/m4-post-login-activation/contract.json), SHA-256 `14f2f89dc150bbe4448e50a02da5e2db4b9ac34c719c1c75f7a461b2b0f4db45`. EXE SHA-256 `4a861ecb4ac7add9554f8daf8f98ad2c12b9cb6c1865a83e8be808beb46ea8a2`, función `0xdab8a0`/offset`0xdaaca0`, pide categoría3 y `UniqueCaseInsensitive_Callsign`; la tarea `0xbe3f00` exige estado autenticado. Meteor SHA-256 `f63fef02ffb517ca7844a769d03412ad82bd22198397302ee3c090c63a970c57`, constructor `0xab530` y ejecutor `0xaf370`/offset`0xae770`, construyen la URI; parser `0xafa50`/offset`0xaee50` espera Result objeto y conserva strings. El filtro de claves es local. No acredita acceso público sin grant ni un nickname funcional en el cliente. El mismo cierre demuestra una llamada PlayFab síncrona cuyo retorno es ignorado para enum1; MeteorURI no redirige esa base histórica.

## Alcance y evidencia

Revisión autorizada del 2026-10-11: el cliente original se ejecutó en ventana1280×720 con Steam abierto, una DLL auxiliar propia y un bloqueo temporal de destinos externos del juego. **CONFIRMADO runtime:** el PID17580 cargó la DLL propia, estableció TCP hacia127.0.0.1:80 y conservó `http://127.0.0.1` en el FString global RVA `0x1d39010`. EXE SHA-256 `4a861ecb4ac7add9554f8daf8f98ad2c12b9cb6c1865a83e8be808beb46ea8a2`. Se leyeron únicamente16bytes de descriptor y34bytes del valor público esperado; no la línea de comandos ni tickets. [Valor público](../../implementation/evidence/local-steam-routing-20261011-051458/public-uri-runtime.json), [socket del cliente](../../implementation/evidence/local-steam-routing-20261011-051458/client-connection.json) y [módulo](../../implementation/evidence/local-steam-routing-20261011-051458/client-module.json). El arranque usado fue el EXE original con Steam existente tras fallar la nueva invocación de Steam por su plugin Millennium; no se atribuye éxito a esa invocación. El cliente mostró “Server not available”, sin login ni partida. El helper terminó FAILED por una carrera al observar el proceso saliente; la retirada de archivos/regla y los cinco hashes originales sí se comprobaron. El alcance estático histórico descrito a continuación conserva su fecha.

El requisito posterior autoriza DLL propia y TOML junto al cliente, sin launcher ni cambios en originales en disco. Esta evidencia sustituye la limitación runtime del override; editar sólo el INI continúa contradicho en el camino estático observado. La URI es la base Meteor única: no prueba que el Master de directorio deba almacenar cuentas.

Original tratado como solo lectura: `F:\SteamLibrary\steamapps\common\Hawken`. Se examinaron la DLL Meteor, las bibliotecas indicadas abajo y AutoReporter. El EXE principal y los paquetes UE3 tienen otro análisis: este catálogo no sustituye sus hallazgos. No se ejecutó el juego, no se consultó DNS, no se contactaron los endpoints y no se usaron credenciales ni capturas.

REA 6.3.0 se diagnosticó por el padre (`evidence/rea-doctor.json`); al comenzar no había herramientas REA MCP conectadas ni proveedor nativo configurado. Se continuó con PE/COFF, `objdump` y scripts Python sin dependencias. AutoReporter está corroborado por REA: `evidence/rea-autoreporter-managed.json`, Evidence ID `ev_36b3bf3514c82e02213fd037cc4405675c859246baf454511818dd12d04c7e6d`, clasificación CIL/managed. Ghidra 12.1.4 headless directo decompiló funciones seleccionadas con SHA original cotejado en `evidence/ghidra-sdk-functions.jsonl`, `ghidra-sdk-services.jsonl` y `ghidra-sdk-auxiliary.jsonl`; REA 6.3.0 rechazó esta DLL en Windows con `target_role_unsupported` (`evidence/rea-sdk/open-binary.json`). El padre obtuvo la evidencia sin alterar la muestra.

Los offsets son posiciones hexadecimales de archivo; RVA es relativa a la base de imagen. Meteor es PE32+ x64 con ImageBase `0x180000000`. `evidence/auth/*-pe.json` retiene secciones, imports, exports, límites `.pdata`, strings de certificados y candidatos. `meteor-custom-string-xrefs.json` retiene instrucción, RVA, función `.pdata` y número de línea de `meteor-disassembly.txt`. No se interpretan símbolos exportados como prueba de ejecución.

| Archivo | SHA-256 | Strings ASCII/UTF16LE extraídas | Strings candidatas a endpoint |
|---|---|---:|---:|
| `AutoReporter.exe` | `25407fa1b48e1ef43193f1b17b5175adf038787b71da3048a3600d1cbdb79aec` | 908 | 58 |
| `libeay32.dll` | `79cc4b787cc7bd0c99525d84ba75289141a95b675b764ad6f6afe57dbf8ea91a` | 30959 | 36 |
| `Meteor.Sdk.Cpp.Win32.Client64.dll` | `f63fef02ffb517ca7844a769d03412ad82bd22198397302ee3c090c63a970c57` | 27020 | 46 |
| `ortp.dll` | `d1dc517e745759f31e7c55e5573697ae71faa3f0853aee77f50dba3154e70a79` | 3421 | 9 |
| `ortp_x64.dll` | `9023efdd1b4a9cf025d0031961d903636fea9a29b9c2fb16375996378fb61093` | 3609 | 14 |
| `ssleay32.dll` | `6b19070c84e92642fd8daae373309af8b7a3de49de7fc2374ebf5744feb631c9` | 6056 | 25 |
| `steam_api64.dll` | `38f6a1dd1e738e142160322638dae7b88dabcc30eea3f15c58eb08c5341f9b29` | 3752 | 23 |
| `twitchsdk_64_release.dll` | `1ee603670cc5c4a1f946ba82f066c6b7bced3c69729536425a97176156a65b76` | 13370 | 39 |
| `vivoxoal_x64.dll` | `9135f28c0d37cb688b54afe1698e13edd6872a10712be993d90cab0b2b7684e7` | 3530 | 9 |
| `vivoxplatform.dll` | `358fa8a00a8a704b2f6d0731884d1bbfa87a47e518f52378f2ceed9fdc2050e4` | 8457 | 14 |
| `vivoxsdk_x64.dll` | `a0140b99a85083308e9bf337baf7cd78f1a3e975fb4e94869d172a3cf960d9a0` | 56129 | 64 |

## Destinos de Hawken y SDK personalizados

| Destino o familia | Localización/evidencia | Interpretación y estado |
|---|---|---|
| `https://v2-services-live-pc.playhawken.com` | `StormConsumerSettings.ini:4`, offset `0x4d`; hash en `config-observations.json` | CONFIRMADO literal del INI; SDK lo lee durante creación y EXE lo sobrescribe después. INI-only contradicho en ese camino; uso runtime NO VERIFICADO. |
| `https://v2-services-main.playhawken.com` | Meteor offset `0x1a5410`, RVA `0x1a6410`, función `0x74010` | CONFIRMADO literal y referencia; CONFIRMADO default `meteoruri` en constructor `0x74010` por Ghidra; selección runtime pendiente. |
| `https://3F09.playfabapi.com` y dominio construido con `.playfabapi.com` | Meteor `0x1a6cd0`/`0x1a7cd0`, función `0xa4ed0`; sufijo `0x1a6cf0`/`0x1a7cf0`, función `0xa4f20`; TitleId `3F09` | CONFIRMADO host y constructor; endpoint completo combina base/rutas `Client/...`. |
| `http://collect5957hwknp.deltadna.net/collect/api/` + `/bulk` | Meteor `0x1a3be8`/`0x1a4be8` y `0x1a3c1c`/`0x1a4c1c`, misma función `0x53e60` | CONFIRMADO literales y xrefs en emisor de eventos; Ghidra confirma base + campo del consumidor `+0xb0` + `/bulk`; valor dinámico pendiente. |
| `http://localhost:13300/productUpdate/` | Meteor `0x1a54f8`/`0x1a64f8`, función `0x75450` | CONFIRMADO literal de comprobación de actualización; puerto explícito 13300. NO VERIFICADO que se use en el inicio de Hawken. |
| `http://www.playhawken.com` | `DefaultOnlineConfig.ini:2` | CONFIRMADO BaseWebURL de Hawken; no es la base API Meteor. |
| Vivox API/SIP y XMPP | `/vivox/credentials`, `/thirdParty/`, `/Presence/Domain`, `/Presence/Access` | CONFIRMADO rutas y parsing/construcción de código; hosts efectivos provienen de respuestas. No hay dominio de producción recuperado para estos canales. |

## Precedencia del INI y selección del cliente original

**CONFIRMADO flujo estático, selección sólo por INI contradicha en el camino observado:** el archivo distribuido es plano, sin `[MeteorSettings]`. El SDK abre el nombre relativo `StormConsumerSettings.ini` y normaliza `MeteorUri` a `meteoruri`, pero lo hace durante `MeteorCore_Create`, antes de devolver el core al EXE. La cadena es DLL `0x1edd0` → constructor `0x1ee80` → vtable `0x1a2b88`, slot `+0xd0` → `0x1ef50` → lector `0x74220` → parser `0x75920`/`0x75a80`. Después, EXE `0xbc3510` selecciona el literal live-PC cuando la longitud de la string global `0x1d39018` es 0 o 1, o copia el valor global `0x1d39010`; llama slot 0 del core → DLL `0x74810`, que sobrescribe la misma clave `meteoruri`. El slot `+0x28` posterior no vuelve a leer el INI: DLL `0x74c00` → executor `+0x30` → `0x72850`, que activa un byte y retorna. Por tanto, editar únicamente `MeteorUri` no determina la base Meteor al terminar esa inicialización normal del EXE. El uso efectivo del archivo depende además del directorio de trabajo, que queda **NO VERIFICADO**; esa limitación runtime no elimina la contradicción estática.

Evidencia nueva: `evidence/auth/ini-endpoint-flow.json` y `ini-endpoint-disassembly.txt`, con 776 instrucciones cotejadas contra bytes originales, slots de vtable y extractos de las decompilaciones existentes. DLL SHA-256 `f63fef02ffb517ca7844a769d03412ad82bd22198397302ee3c090c63a970c57`; lector RVA/offset `0x74220`/`0x73620`, parser `0x75920`/`0x74d20`, setter `0x74810`/`0x73c10`, factory `0x1edd0`/`0x1e1d0`. EXE SHA-256 `4a861ecb4ac7add9554f8daf8f98ad2c12b9cb6c1865a83e8be808beb46ea8a2`; selector RVA/offset `0xbc3510`/`0xbc2910`. `0xc70e20` (offset `0xc70220`) pasa el literal UTF16 `MeteorURI=` (`0x1710560`/`0x170f960`) al helper `0xaf610` y la global `0x1d39010`; la extracción del valor al FString global queda **CONFIRMADA** por `af610`/`af820`; `b3bd0` (RVA/offset `0xb3bd0`/`0xb2fd0`) omite el primer token entre comillas o hasta el espacio y los espacios siguientes; `c6fa40` (`0xc6fa40`/`0xc6ee40`) llama `c6f890` conservando el argumento en ambas ramas. **CONFIRMADO origen en línea de comandos:** `c77450` (`0xc77450`/`0xc76850`) obtiene `GetCommandLineW` por IAT `0x128e498`, conserva su resultado y lo entrega a esa cadena. Las instrucciones originales resuelven los parámetros omitidos por el decompilador; runtime **NO VERIFICADO**. Evidencia: `evidence/ghidra-main-upk-http-producers.jsonl`, `evidence/auth/ini-override-source-instructions.json` y `ini-endpoint-native-closure.json`, con SHA original EXE cotejado y 342 spans de instrucciones comparados contra el original. Es evidencia histórica del cliente, no una alternativa aprobada al requisito del usuario de selección sólo por INI.

## Rutas construidas en Meteor

Revisión de arranque: [contrato Ghidra](evidence/auth/m4-startup-status/contract.json), SHA-256 `0112613ac28703b34f6c22a4f92b09de8ed046dd674edc4fa14ca39434355661`, coteja2083spans originales sin diferencias. **CONFIRMADO estático:** DLL Meteor SHA-256 `f63fef02ffb517ca7844a769d03412ad82bd22198397302ee3c090c63a970c57`; ctor RVA `0xb6750` compone `/status/` + nombre + `/`, por tanto `/status/services/` y `/status/game_client/`. RVA `0xb5f50` exige `Result` string dentro del envelope. `/version`, ctor `0xbea60` y parser `0xbef10`, exige `Result` objeto con `Version` y `Time` strings. Los constructores Steam `0x42360/0x43a40/0x45350` conservan rutas sin slash final; la cadena `0x746c0`→`0xd98e0` concatena base/ruta y no añade `/api/v1`. Las respuestas locales y su aceptación por Hawken se verifican por separado; una ruta recuperada no acredita autenticación ni una solicitud runtime concreta.

Cada fila es un literal o fragmento observado y referenciado desde código. Los parámetros `{...}` se describen únicamente cuando la función permite reconstruirlos; una barra final no demuestra un esquema completo. La mayoría usa la base `MeteorUri`; `/bulk` pertenece al emisor DeltaDNA. Los nombres de ruta son personalizados, distintos del transporte de sesión stock UE3.

| Literal de ruta/fragmento | Offset / RVA | Funciones con xref directa | Estado |
|---|---|---|---|
| `/gameClientEvent` | 0x1a0680 / 0x1a1680 | 0x4960 | CONFIRMADO literal y xref |
| `/userGameItemsBroker/` | 0x1a0a70 / 0x1a1a70 | 0x9fe0 | CONFIRMADO literal y xref |
| `Client/GetLeaderboard` | 0x1a13f8 / 0x1a23f8 | 0xe7a0 | CONFIRMADO literal y xref |
| `Client/GetLeaderboardAroundPlayer` | 0x1a1410 / 0x1a2410 | 0x10d60, 0xf190 | CONFIRMADO literal y xref |
| `Client/WritePlayerEvent` | 0x1a1538 / 0x1a2538 | 0x11f10 | CONFIRMADO literal y xref |
| `Client/GetTitleData` | 0x1a1590 / 0x1a2590 | 0x13cb0 | CONFIRMADO literal y xref |
| `Client/GetContentDownloadUrl` | 0x1a15d0 / 0x1a25d0 | 0x14f70 | CONFIRMADO literal y xref |
| `Client/ExecuteCloudScript` | 0x1a1680 / 0x1a2680 | 0x15fa0 | CONFIRMADO literal y xref |
| `Client/UpdateUserData` | 0x1a16d8 / 0x1a26d8 | 0x16910 | CONFIRMADO literal y xref |
| `Client/GetUserData` | 0x1a1718 / 0x1a2718 | 0x17ec0 | CONFIRMADO literal y xref |
| `/steam-offers/client-offers` | 0x1a1948 / 0x1a2948 | 0x1bc00 | CONFIRMADO literal y xref |
| `steam/initiate-transaction` | 0x1a19d0 / 0x1a29d0 | 0x1cde0 | CONFIRMADO literal y xref |
| `steam/finalize-transaction` | 0x1a1a28 / 0x1a2a28 | 0x1dc20 | CONFIRMADO literal y xref |
| `/achievements` | 0x1a24c0 / 0x1a34c0 | 0x2a2e0, 0x2c360 | CONFIRMADO literal y xref |
| `/achievementsBatch` | 0x1a24e0 / 0x1a34e0 | 0x2b490 | CONFIRMADO literal y xref |
| `/userAchievements` | 0x1a2588 / 0x1a3588 | 0x2c880 | CONFIRMADO literal y xref |
| `/userAchievementsQuery/` | 0x1a25a0 / 0x1a35a0 | 0x2d920 | CONFIRMADO literal y xref |
| `/userClientAchievements` | 0x1a2600 / 0x1a3600 | 0x2eaf0 | CONFIRMADO literal y xref |
| `/userRerollAssignments` | 0x1a2630 / 0x1a3630 | 0x2fbb0 | CONFIRMADO literal y xref |
| `/userDailyAssignments` | 0x1a2670 / 0x1a3670 | 0x30c90 | CONFIRMADO literal y xref |
| `/achievementReward/` | 0x1a2700 / 0x1a3700 | 0x32c20, 0x33360 | CONFIRMADO literal y xref |
| `/achievementRewardBatch/` | 0x1a27b8 / 0x1a37b8 | 0x340c0 | CONFIRMADO literal y xref |
| `/game-client/achievement-rewards` | 0x1a2800 / 0x1a3800 | 0x34c50 | CONFIRMADO literal y xref |
| `/game-client/achievements/` | 0x1a2830 / 0x1a3830 | 0x35950 | CONFIRMADO literal y xref |
| `/users` | 0x1a3268 / 0x1a4268 | 0x3aac0, 0x3dc60, 0xcecc0 | CONFIRMADO literal y xref |
| `/users/` | 0x1a32e0 / 0x1a42e0 | 0x3c190, 0x3cf90, 0x3dc60, 0x3eec0, 0x40a20, 0x48c00, 0xb3670, 0xb45e0, 0xcf900, 0xd0480, 0xd1200 | CONFIRMADO literal y xref |
| `/deleteToken` | 0x1a3310 / 0x1a4310 | 0x3dc60 | CONFIRMADO literal y xref |
| `/accessGrant` | 0x1a3408 / 0x1a4408 | 0x40a20, 0x48c00 | CONFIRMADO literal y xref |
| `/steam/login` | 0x1a3470 / 0x1a4470 | 0x42360 | CONFIRMADO literal y xref |
| `/steam/create-account` | 0x1a3480 / 0x1a4480 | 0x43a40 | CONFIRMADO literal y xref |
| `/steam/link-account` | 0x1a3500 / 0x1a4500 | 0x45350 | CONFIRMADO literal y xref |
| `/steam/client-dlc-installed` | 0x1a3518 / 0x1a4518 | 0x464c0 | CONFIRMADO literal y xref |
| `/antiAddictionModNumber/` | 0x1a3590 / 0x1a4590 | 0x477d0, 0x486c0 | CONFIRMADO literal y xref |
| `/bundles/` | 0x1a3790 / 0x1a4790 | 0x4c2c0, 0x4d3b0, 0x4e4d0 | CONFIRMADO literal y xref |
| `/redeemBundles/` | 0x1a3828 / 0x1a4828 | 0x4f0e0 | CONFIRMADO literal y xref |
| `/bulk` | 0x1a3c1c / 0x1a4c1c | 0x53e60 | CONFIRMADO literal y xref |
| `/game-client/game-items` | 0x1a4898 / 0x1a5898 | 0x62610 | CONFIRMADO literal y xref |
| `/gameItems/` | 0x1a4920 / 0x1a5920 | 0x63350 | CONFIRMADO literal y xref |
| `/gameItemsBatch` | 0x1a4958 / 0x1a5958 | 0x647f0 | CONFIRMADO literal y xref |
| `/userGameItems/` | 0x1a4988 / 0x1a5988 | 0x657e0, 0x665b0 | CONFIRMADO literal y xref |
| `/userGameItemsBatch/` | 0x1a49f8 / 0x1a59f8 | 0x66e60 | CONFIRMADO literal y xref |
| `/userGameItemStats/` | 0x1a4ac0 / 0x1a5ac0 | 0x68990, 0x69620, 0x6ac10, 0x6bc20 | CONFIRMADO literal y xref |
| `/gameServerListings/` | 0x1a56b8 / 0x1a66b8 | 0x7de00 | CONFIRMADO literal y xref |
| `/gameServerListings` | 0x1a56d8 / 0x1a66d8 | 0x7f0d0 | CONFIRMADO literal y xref |
| `/userGameServers/` | 0x1a5730 / 0x1a6730 | 0x806b0 | CONFIRMADO literal y xref |
| `/hawkenClientMatchmakingAdvertisements` | 0x1a57b0 / 0x1a67b0 | 0x80e80, 0x822e0, 0x82d40 | CONFIRMADO literal y xref |
| `/session` | 0x1a5850 / 0x1a6850 | 0x83b30 | CONFIRMADO literal y xref |
| `/gameOffers` | 0x1a5ea0 / 0x1a6ea0 | 0x88ee0, 0x8c060 | CONFIRMADO literal y xref |
| `/gameOffersBatch` | 0x1a5ec0 / 0x1a6ec0 | 0x8a5f0 | CONFIRMADO literal y xref |
| `/game-client/game-offers` | 0x1a5ef8 / 0x1a6ef8 | 0x8baa0 | CONFIRMADO literal y xref |
| `/gameItemOffers` | 0x1a5f40 / 0x1a6f40 | 0x8c060 | CONFIRMADO literal y xref |
| `/gameItemOffersFiltered` | 0x1a5f78 / 0x1a6f78 | 0x8c6b0, 0x8e760 | CONFIRMADO literal y xref |
| `/userGameOfferRedeemer` | 0x1a6060 / 0x1a7060 | 0x90480 | CONFIRMADO literal y xref |
| `/userGameOfferRenter` | 0x1a6088 / 0x1a7088 | 0x91060 | CONFIRMADO literal y xref |
| `/adminGameOfferRedeemer` | 0x1a60c0 / 0x1a70c0 | 0x91db0 | CONFIRMADO literal y xref |
| `Client/LoginWithCustomID` | 0x1a6d18 / 0x1a7d18 | 0xa50c0 | CONFIRMADO literal y xref |
| `Client/UpdateUserTitleDisplayName` | 0x1a6d50 / 0x1a7d50 | 0xa5950 | CONFIRMADO literal y xref |
| `Client/AddGenericID` | 0x1a6da8 / 0x1a7da8 | 0xa5be0 | CONFIRMADO literal y xref |
| `/gameCurrency` | 0x1a6dd8 / 0x1a7dd8 | 0xa6aa0 | CONFIRMADO literal y xref |
| `/meteorCurrency` | 0x1a6e28 / 0x1a7e28 | 0xa7210 | CONFIRMADO literal y xref |
| `/uniqueValues/` | 0x1a70b0 / 0x1a80b0 | 0xae740 | CONFIRMADO literal y xref |
| `/userReadEula/` | 0x1a71a0 / 0x1a81a0 | 0xb00d0, 0xb13d0 | CONFIRMADO literal y xref |
| `/statOverflow/` | 0x1a7548 / 0x1a8548 | 0xb2f30 | CONFIRMADO literal y xref |
| `/statTransfer/` | 0x1a75a8 / 0x1a85a8 | 0xb3670, 0xb45e0 | CONFIRMADO literal y xref |
| `/status/` | 0x1a7680 / 0x1a8680 | 0xb6750 | CONFIRMADO literal y xref |
| `/userStats/` | 0x1a78c0 / 0x1a88c0 | 0xb8ce0, 0xb9840, 0xbadc0, 0xbc200, 0xbd000 | CONFIRMADO literal y xref |
| `/userStats` | 0x1a7948 / 0x1a8948 | 0xbc4f0 | CONFIRMADO literal y xref |
| `/version` | 0x1a79b0 / 0x1a89b0 | 0xbea60 | CONFIRMADO literal y xref |
| `/vivox/credentials` | 0x1a7bd0 / 0x1a8bd0 | 0xc01b0 | CONFIRMADO literal y xref |
| `sip:confctl-2@` | 0x1a7fb0 / 0x1a8fb0 | 0xc2290 | CONFIRMADO literal y xref |
| `/voiceChannelListings/` | 0x1a7ff0 / 0x1a8ff0 | 0xc4880 | CONFIRMADO literal y xref |
| `sip:` | 0x1a8080 / 0x1a9080 | 0xc5a30 | CONFIRMADO literal y xref |
| `/thirdParty/` | 0x1a8488 / 0x1a9488 | 0xc9410 | CONFIRMADO literal y xref |
| `/Presence/Domain` | 0x1a8498 / 0x1a9498 | 0xc9410 | CONFIRMADO literal y xref |
| `/Presence/Access` | 0x1a8568 / 0x1a9568 | 0xc9410 | CONFIRMADO literal y xref |
| `/clans` | 0x1a86b4 / 0x1a96b4 | 0xcd4f0, 0xd1200 | CONFIRMADO literal y xref |
| `/clans/` | 0x1a8700 / 0x1a9700 | 0xce260, 0xcecc0, 0xcf900, 0xd0480, 0xd4170, 0xd4880, 0xd4e00, 0xd5380, 0xd58f0 | CONFIRMADO literal y xref |
| `/clans?clanName=` | 0x1a87e0 / 0x1a97e0 | 0xd20f0 | CONFIRMADO literal y xref |
| `/clans?clanTag=` | 0x1a8890 / 0x1a9890 | 0xd2ef0 | CONFIRMADO literal y xref |
| `/userGameTransaction` | 0x1a89a8 / 0x1a99a8 | 0xd7570, 0xd7b40, 0xd8d90 | CONFIRMADO literal y xref |

### Reconstrucciones específicas

| URI lógica | Evidencia estática | Estado |
|---|---|---|
| `{MeteorUri}/userGameTransaction/{user escapado}` (POST) y `/userGameTransaction/{user escapado}/{transaction escapada}` (GET) | SDK SHA `f63fef02ffb517ca7844a769d03412ad82bd22198397302ee3c090c63a970c57`: POST constructor/ejecutor `0xd7b40`, asíncrono `0xd7570`→`0xd6f00`; GET `0xd8d90` concatena dos segmentos y `0xd8650` ejecuta. [Cierre postmatch](../../implementation/evidence/postmatch-sync-contract/contract.json). | CONFIRMADO estático; GET usado al tratar errores de PUT de estadísticas. No demuestra bloqueo de UI esperando una transacción. |
| `{MeteorUri}/users/{email escapado}/accessGrant` | `0x40a20` concatena `/users/`, transforma email con `0x6fd50`, añade `/accessGrant`; ejecutor `0x401b0` usa POST. | CONFIRMADO construcción/POST; CONFIRMADO percent-encoding doble: A-Z/a-z/0-9/-._~ conservados, resto %XX uppercase, helper `0xf0850`, whitelist `auth/uri-escape-whitelist.json`. |
| `{MeteorUri}/steam/login`, `/steam/create-account`, `/steam/link-account`, `/steam/client-dlc-installed` | Constructores `0x42360`, `0x43a40`, `0x45350`, `0x464c0`; ejecutores POST `0x417a0`, `0x42ea0`, `0x44680`, `0x45f40`. | CONFIRMADO código, sin prueba runtime. |
| `{base PlayFab}/Client/LoginWithCustomID` | `0xa50c0`, helper de URL `0xa4fc0`, POST en `0xa5392`; TitleId/CustomId/CreateAccount. | CONFIRMADO ruta y verbo. |
| `{MeteorUri}/thirdParty/{valor dinámico}/Presence/Domain` y `.../Presence/Access` | `0xc9410` usa los tres fragmentos, procesa resultados y construye cliente XMPP. | CONFIRMADO GET y Result interno string en ambas rutas por Ghidra; valor de canal/host y semántica del access dinámicos pendientes. |
| `sip:confctl-2@{realm}` y `sip:{valor dinámico}` | fragmentos `0x1a7fb0`/`0x1a8fb0`, `0x1a8080`/`0x1a9080`; xrefs completas en JSON. | CONFIRMADO construcción por fragmentos; host proviene de credenciales. |
| JID de party con `@party.` | Meteor offset `0x1a8cd0`, RVA `0x1a9cd0` | CONFIRMADO sufijo; dominio completo dinámico NO VERIFICADO. |

## Puertos, IP y servicios de terceros

Puertos explícitos de los literales: Meteor localhost **13300**; AutoReporter localhost **49418** y sitio crashreport **81**; fixture SIP Vivox **28630** y **5060**. Las IP del fixture (`192.168.0.33`, `69.90.168.6`, `69.90.155.77`) no se atribuyen a servidores de Hawken. `0.0.0.0`, loopback y redes privadas de las bibliotecas son lógica genérica/fixtures. `198.41.0.4` y `a.root-servers.net` aparecen en componentes Vivox como infraestructura de resolución candidata, no como backend del juego.

La función HTTP Meteor `0x6ed50` usa WinHTTP. **CONFIRMADO comportamiento del API:** `WinHttpConnect` recibe puerto **0**, que el header Windows SDK 10.0.26100.0 define como `INTERNET_DEFAULT_PORT`; selecciona 80 para HTTP y 443 para HTTPS. El flag `WINHTTP_FLAG_SECURE=0x00800000` en `WinHttpOpenRequest` determina la transacción HTTPS. `evidence/winhttp-default-ports.json` enlaza los macros del header `winhttp.h` (SHA-256 `45b2ccf904eeaa904e773e90a1e677bd04e41fb57e003f3f398cde0195c2fdd2`) con la DLL Meteor/hash y las instrucciones RVA `0x6f0a8`/call `0x6f0b3`; el flag se prepara en `0x6f297`, call `0x6f2cf`. Estos puertos son la semántica confirmada de ese camino del API; conexión real, servicio activo y soporte de puertos explícitos en URI siguen **NO VERIFICADOS**. [Microsoft: WinHttpConnect](https://learn.microsoft.com/en-us/windows/win32/api/winhttp/nf-winhttp-winhttpconnect). Los puertos XMPP, RTP/SIP y Steam efectivos quedan NO VERIFICADOS.

Vivox incluye `https://watson.vivox.com` (offset `0x42d578`) y muestras SIP `bnw.libretel.com`. Twitch incluye `https://api.twitch.tv/kraken`, OAuth2 `/kraken/oauth2/token`, streams, ingests, búsqueda de juegos, metadata/events, comerciales, canales, usuario, emoticons/badges e `irc.twitch.tv`. Estos son endpoints de terceros distribuídos; no demuestran que Hawken active streaming o contacte Watson.

Steam API expone funciones de ticket y servicios Steam, pero en esta DLL no se recuperó un host de backend Steam atribuible al juego. Sus URLs CA son certificados. La autenticación Steam de Hawken visible se entrega a Meteor en `/steam/...`; no se confunde con `OnlineSubsystemSteamworks`, deseleccionado en DefaultEngine.ini.

## Crash reporter y configuración de desarrollo

| Literal | Archivo/offset | Interpretación |
|---|---|---|
| `http://autoreportservice.meteor-ent.com/UploadReportFiles.aspx` | AutoReporter `0xb7ac` | CONFIRMADO atributo/settings distribuido; transmisión NO VERIFICADA. |
| `http://autoreportservice.meteor-ent.com/RegisterReport.asmx` | AutoReporter `0xb8a8` | CONFIRMADO candidato SOAP del crash reporter. |
| `http://crashreport.meteor-ent.com:81/Crashes/` | AutoReporter `0xb8e9` | CONFIRMADO sitio de consulta de reportes. |
| `http://localhost:49418/AutoReportService/RegisterReport.asmx` y `/UploadReportFiles.aspx` | AutoReporter `0xb800`, `0xb863` | CONFIRMADO alternativas locales de desarrollo. |
| `http://CrashReport/Crashes/`; `meteor-ent.com`, `meteor.lan`, `meteor-ent.com:81` | AutoReporter `0xb842`, UTF16LE `0xa57c`, `0xa59a`, `0xa997` | CONFIRMADO literales; selección por código y uso quedan pendientes. |
| `http://tempuri.org/CreateNewCrash`, `.../AddCrashDescription` | AutoReporter ASCII `0xbafc`, `0xbcd6`; atributos SOAP REA | Namespaces/SOAP actions, no hosts de destino demostrados. |
| `DISINTEGRATOR.METEOR.LAN`, `http://matchreport` | `DefaultEngine.ini:395-402`, `DefaultEditor.ini:68,92` | Config de métricas SQL/editor, PROBABLE desarrollo; no backend de login. |

## Inventario literal completo del ámbito binario

La tabla conserva cada string candidato con su posición, incluso namespaces, créditos, versiones, fixtures y texto DER contiguo. La clasificación impide convertirlos automáticamente en destinos. El JSON `endpoint-ledger.json` conserva las coincidencias por tipo/posición dentro de la string, SHA-256 por fila y xrefs; para certificados usar las filas `DER-*`, que recortan exactamente la longitud ASN.1. Las filas ASCII de certificados pueden contener bytes DER imprimibles al final: ese sufijo no forma parte de la URI.

### AutoReporter.exe

Identidad SHA-256: `25407fa1b48e1ef43193f1b17b5175adf038787b71da3048a3600d1cbdb79aec`. Evidencia: `evidence/auth/AutoReporter.exe-pe.json`.

| Offset / RVA | Encoding | Texto literal | Interpretación/estado |
|---|---|---|---|
| 0x29d0 / 0x47d0 | ASCII | `lSystem.Resources.ResourceReader, mscorlib, Version=2.0.0.0, Culture=neutral, PublicKeyToken=b77a5c561934e089#System.Resources.RuntimeResourceSet` | versión/metadato; no es endpoint demostrado |
| 0x2a6d / 0x486d | ASCII | `fSystem.Drawing.Icon, System.Drawing, Version=2.0.0.0, Culture=neutral, PublicKeyToken=b03f5f7f11d50a3a` | versión/metadato; no es endpoint demostrado |
| 0x2b2a / 0x492a | ASCII | `QSystem.Drawing, Version=2.0.0.0, Culture=neutral, PublicKeyToken=b03f5f7f11d50a3a` | versión/metadato; no es endpoint demostrado |
| 0x4068 / 0x5e68 | ASCII | `lSystem.Resources.ResourceReader, mscorlib, Version=2.0.0.0, Culture=neutral, PublicKeyToken=b77a5c561934e089#System.Resources.RuntimeResourceSet` | versión/metadato; no es endpoint demostrado |
| 0x4105 / 0x5f05 | ASCII | `fSystem.Drawing.Icon, System.Drawing, Version=2.0.0.0, Culture=neutral, PublicKeyToken=b03f5f7f11d50a3a` | versión/metadato; no es endpoint demostrado |
| 0x41b2 / 0x5fb2 | ASCII | `QSystem.Drawing, Version=2.0.0.0, Culture=neutral, PublicKeyToken=b03f5f7f11d50a3a` | versión/metadato; no es endpoint demostrado |
| 0x56f0 / 0x74f0 | ASCII | `lSystem.Resources.ResourceReader, mscorlib, Version=2.0.0.0, Culture=neutral, PublicKeyToken=b77a5c561934e089#System.Resources.RuntimeResourceSet` | versión/metadato; no es endpoint demostrado |
| 0x87b0 / 0xa5b0 | ASCII | `System.IO` | identificador .NET; candidato léxico |
| 0x995e / 0xb75e | ASCII | `System.Net` | identificador .NET; candidato léxico |
| 0xa57c / 0xc37c | UTF16LE | `meteor-ent.com` | tercero/literal; CONFIRMADO existencia; uso runtime NO VERIFICADO |
| 0xa59a / 0xc39a | UTF16LE | `meteor.lan` | tercero/literal; CONFIRMADO existencia; uso runtime NO VERIFICADO |
| 0xa997 / 0xc797 | UTF16LE | `meteor-ent.com:81` | tercero/literal; CONFIRMADO existencia; uso runtime NO VERIFICADO |
| 0xb795 / 0xd595 | ASCII | `10.0.0.0` | infraestructura/red local o ejemplo genérico; NO VERIFICADO en Hawken |
| 0xb7ab / 0xd5ab | ASCII | `>http://autoreportservice.meteor-ent.com/UploadReportFiles.aspx` | tercero/literal; CONFIRMADO existencia; uso runtime NO VERIFICADO |
| 0xb7ff / 0xd5ff | ASCII | `<http://localhost:49418/AutoReportService/RegisterReport.asmx` | tercero/literal; CONFIRMADO existencia; uso runtime NO VERIFICADO |
| 0xb842 / 0xd642 | ASCII | `http://CrashReport/Crashes/` | tercero/literal; CONFIRMADO existencia; uso runtime NO VERIFICADO |
| 0xb862 / 0xd662 | ASCII | `?http://localhost:49418/AutoReportService/UploadReportFiles.aspx` | tercero/literal; CONFIRMADO existencia; uso runtime NO VERIFICADO |
| 0xb8a7 / 0xd6a7 | ASCII | `;http://autoreportservice.meteor-ent.com/RegisterReport.asmx` | tercero/literal; CONFIRMADO existencia; uso runtime NO VERIFICADO |
| 0xb8e8 / 0xd6e8 | ASCII | `-http://crashreport.meteor-ent.com:81/Crashes/` | tercero/literal; CONFIRMADO existencia; uso runtime NO VERIFICADO |
| 0xba00 / 0xd800 | ASCII | `4.0.0.0` | tercero/literal; CONFIRMADO existencia; uso runtime NO VERIFICADO |
| 0xbaaf / 0xd8af | ASCII | `http://tempuri.org/` | namespace/documentación; NO VERIFICADO como destino runtime |
| 0xbafb / 0xd8fb | ASCII | `!http://tempuri.org/CreateNewCrash` | namespace/documentación; NO VERIFICADO como destino runtime |
| 0xbb33 / 0xd933 | ASCII | `http://tempuri.org/T` | namespace/documentación; NO VERIFICADO como destino runtime |
| 0xbb5b / 0xd95b | ASCII | `http://tempuri.org/TU` | namespace/documentación; NO VERIFICADO como destino runtime |
| 0xbb72 / 0xd972 | ASCII | `System.Web.Services.Description.SoapBindingUse, System.Web.Services, Version=2.0.0.0, Culture=neutral, PublicKeyToken=b03f5f7f11d50a3a` | versión/metadato; no es endpoint demostrado |
| 0xbc04 / 0xda04 | ASCII | `System.Web.Services.Protocols.SoapParameterStyle, System.Web.Services, Version=2.0.0.0, Culture=neutral, PublicKeyToken=b03f5f7f11d50a3a` | versión/metadato; no es endpoint demostrado |
| 0xbcd5 / 0xdad5 | ASCII | `&http://tempuri.org/AddCrashDescription` | namespace/documentación; NO VERIFICADO como destino runtime |
| 0xbd12 / 0xdb12 | ASCII | `http://tempuri.org/T` | namespace/documentación; NO VERIFICADO como destino runtime |
| 0xbd3a / 0xdb3a | ASCII | `http://tempuri.org/TU` | namespace/documentación; NO VERIFICADO como destino runtime |
| 0xbd51 / 0xdb51 | ASCII | `System.Web.Services.Description.SoapBindingUse, System.Web.Services, Version=2.0.0.0, Culture=neutral, PublicKeyToken=b03f5f7f11d50a3a` | versión/metadato; no es endpoint demostrado |
| 0xbde3 / 0xdbe3 | ASCII | `System.Web.Services.Protocols.SoapParameterStyle, System.Web.Services, Version=2.0.0.0, Culture=neutral, PublicKeyToken=b03f5f7f11d50a3a` | versión/metadato; no es endpoint demostrado |
| 0xc6f3 / 0xe4f3 | ASCII | `<asmv1:assembly manifestVersion="1.0" xmlns="urn:schemas-microsoft-com:asm.v1" xmlns:asmv1="urn:schemas-microsoft-com:asm.v1" xmlns:asmv2="urn:schemas-microsoft-com:asm.v2" xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance">` | versión/metadato; no es endpoint demostrado |
| 0xc7d8 / 0xe5d8 | ASCII | `  <assemblyIdentity version="1.0.0.0" name="MyApplication.app" />` | tercero/literal; CONFIRMADO existencia; uso runtime NO VERIFICADO |
| 0xcfbc / 0xedbc | ASCII | `          version="6.0.0.0"` | tercero/literal; CONFIRMADO existencia; uso runtime NO VERIFICADO |
| 0xd742 / sin RVA | ASCII | `http://ocsp.thawte.com0` | certificado/firma distribuida; NO VERIFICADO como destino runtime |
| 0xd77e / sin RVA | ASCII | `.http://crl.thawte.com/ThawteTimestampingCA.crl0` | certificado/firma distribuida; NO VERIFICADO como destino runtime |
| 0xdb23 / sin RVA | ASCII | `http://ts-ocsp.ws.symantec.com07` | certificado/firma distribuida; NO VERIFICADO como destino runtime |
| 0xdb4e / sin RVA | ASCII | `+http://ts-aia.ws.symantec.com/tss-ca-g2.cer0<` | certificado/firma distribuida; NO VERIFICADO como destino runtime |
| 0xdb8c / sin RVA | ASCII | `+http://ts-crl.ws.symantec.com/tss-ca-g2.crl0(` | certificado/firma distribuida; NO VERIFICADO como destino runtime |
| 0xde1d / sin RVA | ASCII | `GoDaddy.com, Inc.1301` | certificado/firma distribuida; NO VERIFICADO como destino runtime |
| 0xde38 / sin RVA | ASCII | `*http://certificates.godaddy.com/repository100.` | certificado/firma distribuida; NO VERIFICADO como destino runtime |
| 0xe048 / sin RVA | ASCII | `http://ocsp.godaddy.com0F` | certificado/firma distribuida; NO VERIFICADO como destino runtime |
| 0xe071 / sin RVA | ASCII | `5http://certificates.godaddy.com/repository/gdroot.crl0K` | certificado/firma distribuida; NO VERIFICADO como destino runtime |
| 0xe0c9 / sin RVA | ASCII | `*http://certificates.godaddy.com/repository0` | certificado/firma distribuida; NO VERIFICADO como destino runtime |
| 0xe27f / sin RVA | ASCII | `GoDaddy.com, Inc.1301` | certificado/firma distribuida; NO VERIFICADO como destino runtime |
| 0xe29a / sin RVA | ASCII | `*http://certificates.godaddy.com/repository100.` | certificado/firma distribuida; NO VERIFICADO como destino runtime |
| 0xe518 / sin RVA | ASCII | `"http://crl.godaddy.com/gds5-16.crl0S` | certificado/firma distribuida; NO VERIFICADO como destino runtime |
| 0xe564 / sin RVA | ASCII | `+http://certificates.godaddy.com/repository/0` | certificado/firma distribuida; NO VERIFICADO como destino runtime |
| 0xe5af / sin RVA | ASCII | `http://ocsp.godaddy.com/0J` | certificado/firma distribuida; NO VERIFICADO como destino runtime |
| 0xe5d4 / sin RVA | ASCII | `>http://certificates.godaddy.com/repository/gd_intermediate.crt0` | certificado/firma distribuida; NO VERIFICADO como destino runtime |
| 0xe7b7 / sin RVA | ASCII | `GoDaddy.com, Inc.1301` | certificado/firma distribuida; NO VERIFICADO como destino runtime |
| 0xe7d2 / sin RVA | ASCII | `*http://certificates.godaddy.com/repository100.` | certificado/firma distribuida; NO VERIFICADO como destino runtime |
| 0xde1d / sin RVA | DER-UTF8/ASCII | `GoDaddy.com, Inc.` | certificado/firma distribuida; NO VERIFICADO como destino runtime |
| 0xde39 / sin RVA | DER-UTF8/ASCII | `http://certificates.godaddy.com/repository` | certificado/firma distribuida; NO VERIFICADO como destino runtime |
| 0xe27f / sin RVA | DER-UTF8/ASCII | `GoDaddy.com, Inc.` | certificado/firma distribuida; NO VERIFICADO como destino runtime |
| 0xe29b / sin RVA | DER-UTF8/ASCII | `http://certificates.godaddy.com/repository` | certificado/firma distribuida; NO VERIFICADO como destino runtime |
| 0xe7b7 / sin RVA | DER-UTF8/ASCII | `GoDaddy.com, Inc.` | certificado/firma distribuida; NO VERIFICADO como destino runtime |
| 0xe7d3 / sin RVA | DER-UTF8/ASCII | `http://certificates.godaddy.com/repository` | certificado/firma distribuida; NO VERIFICADO como destino runtime |

### libeay32.dll

Identidad SHA-256: `79cc4b787cc7bd0c99525d84ba75289141a95b675b764ad6f6afe57dbf8ea91a`. Evidencia: `evidence/auth/libeay32-pe.json`.

| Offset / RVA | Encoding | Texto literal | Interpretación/estado |
|---|---|---|---|
| 0xd80d0 / 0xd8cd0 | ASCII | `SHA1 block transform for x86_64, CRYPTOGAMS by <appro@openssl.org>` | crédito/documentación de tercero |
| 0xdb666 / 0xdc266 | ASCII | `RC4 for x86_64, CRYPTOGAMS by <appro@openssl.org>` | crédito/documentación de tercero |
| 0xdf840 / 0xe0440 | ASCII | `AES for x86_64, CRYPTOGAMS by <appro@openssl.org>` | crédito/documentación de tercero |
| 0xe6680 / 0xe7280 | ASCII | `AES for Intel AES-NI, CRYPTOGAMS by <appro@openssl.org>` | crédito/documentación de tercero |
| 0xe7d10 / 0xe8910 | ASCII | `AESNI-CBC+SHA1 stitch for x86_64, CRYPTOGAMS by <appro@openssl.org>` | crédito/documentación de tercero |
| 0xea0fc / 0xeacfc | ASCII | `Camellia for x86_64 by <appro@openssl.org>` | crédito/documentación de tercero |
| 0xeb480 / 0xec080 | ASCII | `GHASH for x86_64, CRYPTOGAMS by <appro@openssl.org>` | crédito/documentación de tercero |
| 0xec724 / 0xed324 | ASCII | `Montgomery Multiplication for x86_64, CRYPTOGAMS by <appro@openssl.org>` | crédito/documentación de tercero |
| 0xed480 / 0xee080 | ASCII | `Montgomery Multiplication with scatter/gather for x86_64, CRYPTOGAMS by <appro@openssl.org>` | crédito/documentación de tercero |
| 0xeda89 / 0xee689 | ASCII | `GF(2^m) Multiplication for x86_64, CRYPTOGAMS by <appro@openssl.org>` | crédito/documentación de tercero |
| 0x14f370 / 0x150f70 | ASCII | `You need to read the OpenSSL FAQ, http://www.openssl.org/support/faq.html` | crédito/documentación de tercero |
| 0x18d158 / 0x193158 | UTF16LE | `The OpenSSL Project, http://www.openssl.org/` | crédito/documentación de tercero |
| 0x192942 / sin RVA | ASCII | `http://ocsp.thawte.com0` | certificado/firma distribuida; NO VERIFICADO como destino runtime |
| 0x19297e / sin RVA | ASCII | `.http://crl.thawte.com/ThawteTimestampingCA.crl0` | certificado/firma distribuida; NO VERIFICADO como destino runtime |
| 0x192d23 / sin RVA | ASCII | `http://ts-ocsp.ws.symantec.com07` | certificado/firma distribuida; NO VERIFICADO como destino runtime |
| 0x192d4e / sin RVA | ASCII | `+http://ts-aia.ws.symantec.com/tss-ca-g2.cer0<` | certificado/firma distribuida; NO VERIFICADO como destino runtime |
| 0x192d8c / sin RVA | ASCII | `+http://ts-crl.ws.symantec.com/tss-ca-g2.crl0(` | certificado/firma distribuida; NO VERIFICADO como destino runtime |
| 0x19301d / sin RVA | ASCII | `GoDaddy.com, Inc.1301` | certificado/firma distribuida; NO VERIFICADO como destino runtime |
| 0x193038 / sin RVA | ASCII | `*http://certificates.godaddy.com/repository100.` | certificado/firma distribuida; NO VERIFICADO como destino runtime |
| 0x193248 / sin RVA | ASCII | `http://ocsp.godaddy.com0F` | certificado/firma distribuida; NO VERIFICADO como destino runtime |
| 0x193271 / sin RVA | ASCII | `5http://certificates.godaddy.com/repository/gdroot.crl0K` | certificado/firma distribuida; NO VERIFICADO como destino runtime |
| 0x1932c9 / sin RVA | ASCII | `*http://certificates.godaddy.com/repository0` | certificado/firma distribuida; NO VERIFICADO como destino runtime |
| 0x19347f / sin RVA | ASCII | `GoDaddy.com, Inc.1301` | certificado/firma distribuida; NO VERIFICADO como destino runtime |
| 0x19349a / sin RVA | ASCII | `*http://certificates.godaddy.com/repository100.` | certificado/firma distribuida; NO VERIFICADO como destino runtime |
| 0x193718 / sin RVA | ASCII | `"http://crl.godaddy.com/gds5-16.crl0S` | certificado/firma distribuida; NO VERIFICADO como destino runtime |
| 0x193764 / sin RVA | ASCII | `+http://certificates.godaddy.com/repository/0` | certificado/firma distribuida; NO VERIFICADO como destino runtime |
| 0x1937af / sin RVA | ASCII | `http://ocsp.godaddy.com/0J` | certificado/firma distribuida; NO VERIFICADO como destino runtime |
| 0x1937d4 / sin RVA | ASCII | `>http://certificates.godaddy.com/repository/gd_intermediate.crt0` | certificado/firma distribuida; NO VERIFICADO como destino runtime |
| 0x1939b7 / sin RVA | ASCII | `GoDaddy.com, Inc.1301` | certificado/firma distribuida; NO VERIFICADO como destino runtime |
| 0x1939d2 / sin RVA | ASCII | `*http://certificates.godaddy.com/repository100.` | certificado/firma distribuida; NO VERIFICADO como destino runtime |
| 0x19301d / sin RVA | DER-UTF8/ASCII | `GoDaddy.com, Inc.` | certificado/firma distribuida; NO VERIFICADO como destino runtime |
| 0x193039 / sin RVA | DER-UTF8/ASCII | `http://certificates.godaddy.com/repository` | certificado/firma distribuida; NO VERIFICADO como destino runtime |
| 0x19347f / sin RVA | DER-UTF8/ASCII | `GoDaddy.com, Inc.` | certificado/firma distribuida; NO VERIFICADO como destino runtime |
| 0x19349b / sin RVA | DER-UTF8/ASCII | `http://certificates.godaddy.com/repository` | certificado/firma distribuida; NO VERIFICADO como destino runtime |
| 0x1939b7 / sin RVA | DER-UTF8/ASCII | `GoDaddy.com, Inc.` | certificado/firma distribuida; NO VERIFICADO como destino runtime |
| 0x1939d3 / sin RVA | DER-UTF8/ASCII | `http://certificates.godaddy.com/repository` | certificado/firma distribuida; NO VERIFICADO como destino runtime |

### Meteor.Sdk.Cpp.Win32.Client64.dll

Identidad SHA-256: `f63fef02ffb517ca7844a769d03412ad82bd22198397302ee3c090c63a970c57`. Evidencia: `evidence/auth/Meteor.Sdk.Cpp.Win32.Client64-pe.json`.

| Offset / RVA | Encoding | Texto literal | Interpretación/estado |
|---|---|---|---|
| 0x191bb8 / 0x192bb8 | ASCII | `xmlns:stream='http://etherx.jabber.org/streams'  xml:lang='` | namespace/documentación; NO VERIFICADO como destino runtime |
| 0x192b80 / 0x193b80 | ASCII | `http://jabber.org/protocol/disco#info` | namespace/documentación; NO VERIFICADO como destino runtime |
| 0x192ba8 / 0x193ba8 | ASCII | `http://jabber.org/protocol/disco#items` | namespace/documentación; NO VERIFICADO como destino runtime |
| 0x192bd0 / 0x193bd0 | ASCII | `http://jabber.org/protocol/disco#publish` | namespace/documentación; NO VERIFICADO como destino runtime |
| 0x192c00 / 0x193c00 | ASCII | `http://jabber.org/protocol/commands` | namespace/documentación; NO VERIFICADO como destino runtime |
| 0x192c28 / 0x193c28 | ASCII | `http://jabber.org/protocol/compress` | namespace/documentación; NO VERIFICADO como destino runtime |
| 0x192c50 / 0x193c50 | ASCII | `http://jabber.org/protocol/offline` | namespace/documentación; NO VERIFICADO como destino runtime |
| 0x192c78 / 0x193c78 | ASCII | `http://jabber.org/protocol/chatstates` | namespace/documentación; NO VERIFICADO como destino runtime |
| 0x192ca0 / 0x193ca0 | ASCII | `http://jabber.org/protocol/amp` | namespace/documentación; NO VERIFICADO como destino runtime |
| 0x192cc0 / 0x193cc0 | ASCII | `http://jabber.org/protocol/ibb` | namespace/documentación; NO VERIFICADO como destino runtime |
| 0x192ce0 / 0x193ce0 | ASCII | `http://jabber.org/protocol/feature-neg` | namespace/documentación; NO VERIFICADO como destino runtime |
| 0x192d08 / 0x193d08 | ASCII | `http://jabber.org/protocol/chatneg` | namespace/documentación; NO VERIFICADO como destino runtime |
| 0x192d30 / 0x193d30 | ASCII | `http://jabber.org/protocol/xhtml-im` | namespace/documentación; NO VERIFICADO como destino runtime |
| 0x192f10 / 0x193f10 | ASCII | `http://jabber.org/protocol/si` | namespace/documentación; NO VERIFICADO como destino runtime |
| 0x192f30 / 0x193f30 | ASCII | `http://jabber.org/protocol/si/profile/file-transfer` | namespace/documentación; NO VERIFICADO como destino runtime |
| 0x192f68 / 0x193f68 | ASCII | `http://jabber.org/protocol/bytestreams` | namespace/documentación; NO VERIFICADO como destino runtime |
| 0x192f90 / 0x193f90 | ASCII | `http://jabber.org/protocol/muc` | namespace/documentación; NO VERIFICADO como destino runtime |
| 0x192fb0 / 0x193fb0 | ASCII | `http://jabber.org/protocol/muc#user` | namespace/documentación; NO VERIFICADO como destino runtime |
| 0x192fd8 / 0x193fd8 | ASCII | `http://jabber.org/protocol/muc#admin` | namespace/documentación; NO VERIFICADO como destino runtime |
| 0x193000 / 0x194000 | ASCII | `http://jabber.org/protocol/muc#unique` | namespace/documentación; NO VERIFICADO como destino runtime |
| 0x193028 / 0x194028 | ASCII | `http://jabber.org/protocol/muc#owner` | namespace/documentación; NO VERIFICADO como destino runtime |
| 0x193050 / 0x194050 | ASCII | `http://jabber.org/protocol/muc#roominfo` | namespace/documentación; NO VERIFICADO como destino runtime |
| 0x193078 / 0x194078 | ASCII | `http://jabber.org/protocol/muc#rooms` | namespace/documentación; NO VERIFICADO como destino runtime |
| 0x1930a0 / 0x1940a0 | ASCII | `http://jabber.org/protocol/muc#request` | namespace/documentación; NO VERIFICADO como destino runtime |
| 0x1930c8 / 0x1940c8 | ASCII | `http://jabber.org/protocol/pubsub` | namespace/documentación; NO VERIFICADO como destino runtime |
| 0x1930f0 / 0x1940f0 | ASCII | `http://jabber.org/protocol/pubsub#errors` | namespace/documentación; NO VERIFICADO como destino runtime |
| 0x193120 / 0x194120 | ASCII | `http://jabber.org/protocol/pubsub#event` | namespace/documentación; NO VERIFICADO como destino runtime |
| 0x193148 / 0x194148 | ASCII | `http://jabber.org/protocol/pubsub#owner` | namespace/documentación; NO VERIFICADO como destino runtime |
| 0x193170 / 0x194170 | ASCII | `http://jabber.org/protocol/caps` | namespace/documentación; NO VERIFICADO como destino runtime |
| 0x193190 / 0x194190 | ASCII | `http://affinix.com/jabber/stream` | namespace/documentación; NO VERIFICADO como destino runtime |
| 0x1931b8 / 0x1941b8 | ASCII | `http://etherx.jabber.org/streams` | namespace/documentación; NO VERIFICADO como destino runtime |
| 0x1932c8 / 0x1942c8 | ASCII | `http://jabber.org/features/iq-auth` | namespace/documentación; NO VERIFICADO como destino runtime |
| 0x1932f0 / 0x1942f0 | ASCII | `http://jabber.org/features/iq-register` | namespace/documentación; NO VERIFICADO como destino runtime |
| 0x193318 / 0x194318 | ASCII | `http://jabber.org/features/compress` | namespace/documentación; NO VERIFICADO como destino runtime |
| 0x193340 / 0x194340 | ASCII | `http://jabber.org/protocol/httpbind` | namespace/documentación; NO VERIFICADO como destino runtime |
| 0x193390 / 0x194390 | ASCII | `http://jabber.org/protocol/nick` | namespace/documentación; NO VERIFICADO como destino runtime |
| 0x193468 / 0x194468 | ASCII | `http://jabber.org/protocol/shim` | namespace/documentación; NO VERIFICADO como destino runtime |
| 0x1934a8 / 0x1944a8 | ASCII | `http://camaya.net/gloox` | namespace/documentación; NO VERIFICADO como destino runtime |
| 0x1934c0 / 0x1944c0 | ASCII | `http://www.xmpp.org/extensions/xep-0136.html#ns` | namespace/documentación; NO VERIFICADO como destino runtime |
| 0x197cec / 0x198cec | ASCII | `# http://curl.haxx.se/rfc/cookie_spec.html` | crédito/documentación de tercero |
| 0x19b0f0 / 0x19c0f0 | ASCII | `ftp://%s:%s@%s` | tercero/literal; CONFIRMADO existencia; uso runtime NO VERIFICADO |
| 0x1a3be8 / 0x1a4be8 | ASCII | `http://collect5957hwknp.deltadna.net/collect/api/` | código SDK personalizado; CONFIRMADO literal y xref; runtime NO VERIFICADO |
| 0x1a5410 / 0x1a6410 | ASCII | `https://v2-services-main.playhawken.com` | código SDK personalizado; CONFIRMADO literal y xref; runtime NO VERIFICADO |
| 0x1a54f8 / 0x1a64f8 | ASCII | `http://localhost:13300/productUpdate/` | código SDK personalizado; CONFIRMADO literal y xref; runtime NO VERIFICADO |
| 0x1a6cd0 / 0x1a7cd0 | ASCII | `https://3F09.playfabapi.com` | código SDK personalizado; CONFIRMADO literal y xref; runtime NO VERIFICADO |
| 0x1a6cf0 / 0x1a7cf0 | ASCII | `.playfabapi.com` | código SDK personalizado; CONFIRMADO literal y xref; runtime NO VERIFICADO |

### ortp.dll

Identidad SHA-256: `d1dc517e745759f31e7c55e5573697ae71faa3f0853aee77f50dba3154e70a79`. Evidencia: `evidence/auth/ortp-pe.json`.

| Offset / RVA | Encoding | Texto literal | Interpretación/estado |
|---|---|---|---|
| 0x38f98 / 0x3a198 | ASCII | `0.0.0.0` | infraestructura/red local o ejemplo genérico; NO VERIFICADO en Hawken |
| 0x39308 / 0x3a508 | ASCII | `Vovida.org 0.94` | crédito/documentación de tercero |
| 0x4d064 / sin RVA | ASCII | `http://ocsp.verisign.com0` | certificado/firma distribuida; NO VERIFICADO como destino runtime |
| 0x4d09c / sin RVA | ASCII | `"http://crl.verisign.com/tss-ca.crl0` | certificado/firma distribuida; NO VERIFICADO como destino runtime |
| 0x4d496 / sin RVA | ASCII | `http://ocsp.verisign.com0` | certificado/firma distribuida; NO VERIFICADO como destino runtime |
| 0x4d4d4 / sin RVA | ASCII | `0http://crl.verisign.com/ThawteTimestampingCA.crl0` | certificado/firma distribuida; NO VERIFICADO como destino runtime |
| 0x4d650 / sin RVA | DER-UTF8/ASCII | `certs@vivox.com` | certificado/firma distribuida; NO VERIFICADO como destino runtime |
| 0x4d752 / sin RVA | DER-UTF8/ASCII | `certs@vivox.com` | certificado/firma distribuida; NO VERIFICADO como destino runtime |
| 0x4da73 / sin RVA | DER-UTF8/ASCII | `certs@vivox.com` | certificado/firma distribuida; NO VERIFICADO como destino runtime |

### ortp_x64.dll

Identidad SHA-256: `9023efdd1b4a9cf025d0031961d903636fea9a29b9c2fb16375996378fb61093`. Evidencia: `evidence/auth/ortp_x64-pe.json`.

| Offset / RVA | Encoding | Texto literal | Interpretación/estado |
|---|---|---|---|
| 0x39950 / 0x3b150 | ASCII | `0.0.0.0` | infraestructura/red local o ejemplo genérico; NO VERIFICADO en Hawken |
| 0x3a078 / 0x3b878 | ASCII | `Vovida.org 0.94` | crédito/documentación de tercero |
| 0x4f726 / sin RVA | ASCII | `http://ocsp.thawte.com0` | certificado/firma distribuida; NO VERIFICADO como destino runtime |
| 0x4f762 / sin RVA | ASCII | `.http://crl.thawte.com/ThawteTimestampingCA.crl0` | certificado/firma distribuida; NO VERIFICADO como destino runtime |
| 0x4fb0d / sin RVA | ASCII | `#http://crl.thawte.com/ThawtePCA.crl0` | certificado/firma distribuida; NO VERIFICADO como destino runtime |
| 0x4fb5f / sin RVA | ASCII | `http://ocsp.thawte.com0` | certificado/firma distribuida; NO VERIFICADO como destino runtime |
| 0x4ffa7 / sin RVA | ASCII | `http://ts-ocsp.ws.symantec.com07` | certificado/firma distribuida; NO VERIFICADO como destino runtime |
| 0x4ffd2 / sin RVA | ASCII | `+http://ts-aia.ws.symantec.com/tss-ca-g2.cer0<` | certificado/firma distribuida; NO VERIFICADO como destino runtime |
| 0x50010 / sin RVA | ASCII | `+http://ts-crl.ws.symantec.com/tss-ca-g2.crl0(` | certificado/firma distribuida; NO VERIFICADO como destino runtime |
| 0x50452 / sin RVA | ASCII | `http://th.symcb.com/th.crl0` | certificado/firma distribuida; NO VERIFICADO como destino runtime |
| 0x504c7 / sin RVA | ASCII | `https://d.symcb.com/cps0%` | certificado/firma distribuida; NO VERIFICADO como destino runtime |
| 0x504ee / sin RVA | ASCII | `https://d.symcb.com/rpa0` | certificado/firma distribuida; NO VERIFICADO como destino runtime |
| 0x50542 / sin RVA | ASCII | `http://th.symcd.com0&` | certificado/firma distribuida; NO VERIFICADO como destino runtime |
| 0x50563 / sin RVA | ASCII | `http://th.symcb.com/th.crt0` | certificado/firma distribuida; NO VERIFICADO como destino runtime |

### ssleay32.dll

Identidad SHA-256: `6b19070c84e92642fd8daae373309af8b7a3de49de7fc2374ebf5744feb631c9`. Evidencia: `evidence/auth/ssleay32-pe.json`.

| Offset / RVA | Encoding | Texto literal | Interpretación/estado |
|---|---|---|---|
| 0x54b58 / 0x58158 | UTF16LE | `The OpenSSL Project, http://www.openssl.org/` | crédito/documentación de tercero |
| 0x56542 / sin RVA | ASCII | `http://ocsp.thawte.com0` | certificado/firma distribuida; NO VERIFICADO como destino runtime |
| 0x5657e / sin RVA | ASCII | `.http://crl.thawte.com/ThawteTimestampingCA.crl0` | certificado/firma distribuida; NO VERIFICADO como destino runtime |
| 0x56923 / sin RVA | ASCII | `http://ts-ocsp.ws.symantec.com07` | certificado/firma distribuida; NO VERIFICADO como destino runtime |
| 0x5694e / sin RVA | ASCII | `+http://ts-aia.ws.symantec.com/tss-ca-g2.cer0<` | certificado/firma distribuida; NO VERIFICADO como destino runtime |
| 0x5698c / sin RVA | ASCII | `+http://ts-crl.ws.symantec.com/tss-ca-g2.crl0(` | certificado/firma distribuida; NO VERIFICADO como destino runtime |
| 0x56c1d / sin RVA | ASCII | `GoDaddy.com, Inc.1301` | certificado/firma distribuida; NO VERIFICADO como destino runtime |
| 0x56c38 / sin RVA | ASCII | `*http://certificates.godaddy.com/repository100.` | certificado/firma distribuida; NO VERIFICADO como destino runtime |
| 0x56e48 / sin RVA | ASCII | `http://ocsp.godaddy.com0F` | certificado/firma distribuida; NO VERIFICADO como destino runtime |
| 0x56e71 / sin RVA | ASCII | `5http://certificates.godaddy.com/repository/gdroot.crl0K` | certificado/firma distribuida; NO VERIFICADO como destino runtime |
| 0x56ec9 / sin RVA | ASCII | `*http://certificates.godaddy.com/repository0` | certificado/firma distribuida; NO VERIFICADO como destino runtime |
| 0x5707f / sin RVA | ASCII | `GoDaddy.com, Inc.1301` | certificado/firma distribuida; NO VERIFICADO como destino runtime |
| 0x5709a / sin RVA | ASCII | `*http://certificates.godaddy.com/repository100.` | certificado/firma distribuida; NO VERIFICADO como destino runtime |
| 0x57318 / sin RVA | ASCII | `"http://crl.godaddy.com/gds5-16.crl0S` | certificado/firma distribuida; NO VERIFICADO como destino runtime |
| 0x57364 / sin RVA | ASCII | `+http://certificates.godaddy.com/repository/0` | certificado/firma distribuida; NO VERIFICADO como destino runtime |
| 0x573af / sin RVA | ASCII | `http://ocsp.godaddy.com/0J` | certificado/firma distribuida; NO VERIFICADO como destino runtime |
| 0x573d4 / sin RVA | ASCII | `>http://certificates.godaddy.com/repository/gd_intermediate.crt0` | certificado/firma distribuida; NO VERIFICADO como destino runtime |
| 0x575b7 / sin RVA | ASCII | `GoDaddy.com, Inc.1301` | certificado/firma distribuida; NO VERIFICADO como destino runtime |
| 0x575d2 / sin RVA | ASCII | `*http://certificates.godaddy.com/repository100.` | certificado/firma distribuida; NO VERIFICADO como destino runtime |
| 0x56c1d / sin RVA | DER-UTF8/ASCII | `GoDaddy.com, Inc.` | certificado/firma distribuida; NO VERIFICADO como destino runtime |
| 0x56c39 / sin RVA | DER-UTF8/ASCII | `http://certificates.godaddy.com/repository` | certificado/firma distribuida; NO VERIFICADO como destino runtime |
| 0x5707f / sin RVA | DER-UTF8/ASCII | `GoDaddy.com, Inc.` | certificado/firma distribuida; NO VERIFICADO como destino runtime |
| 0x5709b / sin RVA | DER-UTF8/ASCII | `http://certificates.godaddy.com/repository` | certificado/firma distribuida; NO VERIFICADO como destino runtime |
| 0x575b7 / sin RVA | DER-UTF8/ASCII | `GoDaddy.com, Inc.` | certificado/firma distribuida; NO VERIFICADO como destino runtime |
| 0x575d3 / sin RVA | DER-UTF8/ASCII | `http://certificates.godaddy.com/repository` | certificado/firma distribuida; NO VERIFICADO como destino runtime |

### steam_api64.dll

Identidad SHA-256: `38f6a1dd1e738e142160322638dae7b88dabcc30eea3f15c58eb08c5341f9b29`. Evidencia: `evidence/auth/steam_api64-pe.json`.

| Offset / RVA | Encoding | Texto literal | Interpretación/estado |
|---|---|---|---|
| 0x388f4 / 0x3e2f4 | UTF16LE | `03.75.32.07` | versión/metadato; no es endpoint demostrado |
| 0x38974 / 0x3e374 | UTF16LE | `01.00.00.01` | versión/metadato; no es endpoint demostrado |
| 0x3a0e1 / sin RVA | ASCII | `http://ocsp.thawte.com0` | certificado/firma distribuida; NO VERIFICADO como destino runtime |
| 0x3a11d / sin RVA | ASCII | `.http://crl.thawte.com/ThawteTimestampingCA.crl0` | certificado/firma distribuida; NO VERIFICADO como destino runtime |
| 0x3a4c2 / sin RVA | ASCII | `http://ts-ocsp.ws.symantec.com07` | certificado/firma distribuida; NO VERIFICADO como destino runtime |
| 0x3a4ed / sin RVA | ASCII | `+http://ts-aia.ws.symantec.com/tss-ca-g2.cer0<` | certificado/firma distribuida; NO VERIFICADO como destino runtime |
| 0x3a52b / sin RVA | ASCII | `+http://ts-crl.ws.symantec.com/tss-ca-g2.crl0(` | certificado/firma distribuida; NO VERIFICADO como destino runtime |
| 0x3a98b / sin RVA | ASCII | `/http://crl3.digicert.com/sha2-assured-cs-g1.crl05` | certificado/firma distribuida; NO VERIFICADO como destino runtime |
| 0x3a9c2 / sin RVA | ASCII | `/http://crl4.digicert.com/sha2-assured-cs-g1.crl0K` | certificado/firma distribuida; NO VERIFICADO como destino runtime |
| 0x3aa1a / sin RVA | ASCII | `https://www.digicert.com/CPS0` | certificado/firma distribuida; NO VERIFICADO como destino runtime |
| 0x3aa5e / sin RVA | ASCII | `http://ocsp.digicert.com0N` | certificado/firma distribuida; NO VERIFICADO como destino runtime |
| 0x3aa83 / sin RVA | ASCII | `Bhttp://cacerts.digicert.com/DigiCertSHA2AssuredIDCodeSigningCA.crt0` | certificado/firma distribuida; NO VERIFICADO como destino runtime |
| 0x3ae96 / sin RVA | ASCII | `http://ocsp.digicert.com0C` | certificado/firma distribuida; NO VERIFICADO como destino runtime |
| 0x3aebb / sin RVA | ASCII | `7http://cacerts.digicert.com/DigiCertAssuredIDRootCA.crt0` | certificado/firma distribuida; NO VERIFICADO como destino runtime |
| 0x3af06 / sin RVA | ASCII | `4http://crl4.digicert.com/DigiCertAssuredIDRootCA.crl0:` | certificado/firma distribuida; NO VERIFICADO como destino runtime |
| 0x3af42 / sin RVA | ASCII | `4http://crl3.digicert.com/DigiCertAssuredIDRootCA.crl0O` | certificado/firma distribuida; NO VERIFICADO como destino runtime |
| 0x3afa0 / sin RVA | ASCII | `https://www.digicert.com/CPS0` | certificado/firma distribuida; NO VERIFICADO como destino runtime |
| 0x39ae0 / sin RVA | DER-UTF8/ASCII | `www.digicert.com` | certificado/firma distribuida; NO VERIFICADO como destino runtime |
| 0x39b67 / sin RVA | DER-UTF8/ASCII | `www.digicert.com` | certificado/firma distribuida; NO VERIFICADO como destino runtime |
| 0x3a734 / sin RVA | DER-UTF8/ASCII | `www.digicert.com` | certificado/firma distribuida; NO VERIFICADO como destino runtime |
| 0x3ac47 / sin RVA | DER-UTF8/ASCII | `www.digicert.com` | certificado/firma distribuida; NO VERIFICADO como destino runtime |
| 0x3acce / sin RVA | DER-UTF8/ASCII | `www.digicert.com` | certificado/firma distribuida; NO VERIFICADO como destino runtime |
| 0x3b15b / sin RVA | DER-UTF8/ASCII | `www.digicert.com` | certificado/firma distribuida; NO VERIFICADO como destino runtime |

### twitchsdk_64_release.dll

Identidad SHA-256: `1ee603670cc5c4a1f946ba82f066c6b7bced3c69729536425a97176156a65b76`. Evidencia: `evidence/auth/twitchsdk_64_release-pe.json`.

| Offset / RVA | Encoding | Texto literal | Interpretación/estado |
|---|---|---|---|
| 0xa8b60 / 0xa9f60 | ASCII | `https://api.twitch.tv/kraken` | tercero/literal; CONFIRMADO existencia; uso runtime NO VERIFICADO |
| 0xa8ce0 / 0xaa0e0 | ASCII | `https://api.twitch.tv/kraken/oauth2/token` | tercero/literal; CONFIRMADO existencia; uso runtime NO VERIFICADO |
| 0xa8e20 / 0xaa220 | ASCII | `https://api.twitch.tv/kraken/streams` | tercero/literal; CONFIRMADO existencia; uso runtime NO VERIFICADO |
| 0xa8f00 / 0xaa300 | ASCII | `https://api.twitch.tv/kraken/ingests` | tercero/literal; CONFIRMADO existencia; uso runtime NO VERIFICADO |
| 0xa8ff8 / 0xaa3f8 | ASCII | `https://api.twitch.tv/kraken/search/games` | tercero/literal; CONFIRMADO existencia; uso runtime NO VERIFICADO |
| 0xa9180 / 0xaa580 | ASCII | `https://api.twitch.tv/metadata/events?broadcast_id=` | tercero/literal; CONFIRMADO existencia; uso runtime NO VERIFICADO |
| 0xa92e8 / 0xaa6e8 | ASCII | `https://api.twitch.tv/kraken/clients/{client_id}/video_status` | tercero/literal; CONFIRMADO existencia; uso runtime NO VERIFICADO |
| 0xa9438 / 0xaa838 | ASCII | `https://api.twitch.tv/kraken/channels{channel_name}/commercial` | tercero/literal; CONFIRMADO existencia; uso runtime NO VERIFICADO |
| 0xa94f8 / 0xaa8f8 | ASCII | `https://api.twitch.tv/kraken/channels/` | tercero/literal; CONFIRMADO existencia; uso runtime NO VERIFICADO |
| 0xa9698 / 0xaaa98 | ASCII | `https://api.twitch.tv/kraken/streams/` | tercero/literal; CONFIRMADO existencia; uso runtime NO VERIFICADO |
| 0xa9800 / 0xaac00 | ASCII | `https://api.twitch.tv/kraken/channel` | tercero/literal; CONFIRMADO existencia; uso runtime NO VERIFICADO |
| 0xa9930 / 0xaad30 | ASCII | `https://api.twitch.tv/kraken/user` | tercero/literal; CONFIRMADO existencia; uso runtime NO VERIFICADO |
| 0xb0f58 / 0xb2358 | ASCII | `https://api.twitch.tv/kraken/chat/emoticons` | tercero/literal; CONFIRMADO existencia; uso runtime NO VERIFICADO |
| 0xb0f88 / 0xb2388 | ASCII | `https://api.twitch.tv/kraken/chat/{channel_name}/badges` | tercero/literal; CONFIRMADO existencia; uso runtime NO VERIFICADO |
| 0xb1740 / 0xb2b40 | ASCII | `irc.twitch.tv` | tercero/literal; CONFIRMADO existencia; uso runtime NO VERIFICADO |
| 0xf5542 / sin RVA | ASCII | `http://ocsp.thawte.com0` | certificado/firma distribuida; NO VERIFICADO como destino runtime |
| 0xf557e / sin RVA | ASCII | `.http://crl.thawte.com/ThawteTimestampingCA.crl0` | certificado/firma distribuida; NO VERIFICADO como destino runtime |
| 0xf5923 / sin RVA | ASCII | `http://ts-ocsp.ws.symantec.com07` | certificado/firma distribuida; NO VERIFICADO como destino runtime |
| 0xf594e / sin RVA | ASCII | `+http://ts-aia.ws.symantec.com/tss-ca-g2.cer0<` | certificado/firma distribuida; NO VERIFICADO como destino runtime |
| 0xf598c / sin RVA | ASCII | `+http://ts-crl.ws.symantec.com/tss-ca-g2.crl0(` | certificado/firma distribuida; NO VERIFICADO como destino runtime |
| 0xf5c1d / sin RVA | ASCII | `GoDaddy.com, Inc.1301` | certificado/firma distribuida; NO VERIFICADO como destino runtime |
| 0xf5c38 / sin RVA | ASCII | `*http://certificates.godaddy.com/repository100.` | certificado/firma distribuida; NO VERIFICADO como destino runtime |
| 0xf5e48 / sin RVA | ASCII | `http://ocsp.godaddy.com0F` | certificado/firma distribuida; NO VERIFICADO como destino runtime |
| 0xf5e71 / sin RVA | ASCII | `5http://certificates.godaddy.com/repository/gdroot.crl0K` | certificado/firma distribuida; NO VERIFICADO como destino runtime |
| 0xf5ec9 / sin RVA | ASCII | `*http://certificates.godaddy.com/repository0` | certificado/firma distribuida; NO VERIFICADO como destino runtime |
| 0xf607f / sin RVA | ASCII | `GoDaddy.com, Inc.1301` | certificado/firma distribuida; NO VERIFICADO como destino runtime |
| 0xf609a / sin RVA | ASCII | `*http://certificates.godaddy.com/repository100.` | certificado/firma distribuida; NO VERIFICADO como destino runtime |
| 0xf6318 / sin RVA | ASCII | `"http://crl.godaddy.com/gds5-16.crl0S` | certificado/firma distribuida; NO VERIFICADO como destino runtime |
| 0xf6364 / sin RVA | ASCII | `+http://certificates.godaddy.com/repository/0` | certificado/firma distribuida; NO VERIFICADO como destino runtime |
| 0xf63af / sin RVA | ASCII | `http://ocsp.godaddy.com/0J` | certificado/firma distribuida; NO VERIFICADO como destino runtime |
| 0xf63d4 / sin RVA | ASCII | `>http://certificates.godaddy.com/repository/gd_intermediate.crt0` | certificado/firma distribuida; NO VERIFICADO como destino runtime |
| 0xf65b7 / sin RVA | ASCII | `GoDaddy.com, Inc.1301` | certificado/firma distribuida; NO VERIFICADO como destino runtime |
| 0xf65d2 / sin RVA | ASCII | `*http://certificates.godaddy.com/repository100.` | certificado/firma distribuida; NO VERIFICADO como destino runtime |
| 0xf5c1d / sin RVA | DER-UTF8/ASCII | `GoDaddy.com, Inc.` | certificado/firma distribuida; NO VERIFICADO como destino runtime |
| 0xf5c39 / sin RVA | DER-UTF8/ASCII | `http://certificates.godaddy.com/repository` | certificado/firma distribuida; NO VERIFICADO como destino runtime |
| 0xf607f / sin RVA | DER-UTF8/ASCII | `GoDaddy.com, Inc.` | certificado/firma distribuida; NO VERIFICADO como destino runtime |
| 0xf609b / sin RVA | DER-UTF8/ASCII | `http://certificates.godaddy.com/repository` | certificado/firma distribuida; NO VERIFICADO como destino runtime |
| 0xf65b7 / sin RVA | DER-UTF8/ASCII | `GoDaddy.com, Inc.` | certificado/firma distribuida; NO VERIFICADO como destino runtime |
| 0xf65d3 / sin RVA | DER-UTF8/ASCII | `http://certificates.godaddy.com/repository` | certificado/firma distribuida; NO VERIFICADO como destino runtime |

### vivoxoal_x64.dll

Identidad SHA-256: `9135f28c0d37cb688b54afe1698e13edd6872a10712be993d90cab0b2b7684e7`. Evidencia: `evidence/auth/vivoxoal_x64-pe.json`.

| Offset / RVA | Encoding | Texto literal | Interpretación/estado |
|---|---|---|---|
| 0x58f42 / sin RVA | ASCII | `http://ocsp.thawte.com0` | certificado/firma distribuida; NO VERIFICADO como destino runtime |
| 0x58f7e / sin RVA | ASCII | `.http://crl.thawte.com/ThawteTimestampingCA.crl0` | certificado/firma distribuida; NO VERIFICADO como destino runtime |
| 0x592d7 / sin RVA | ASCII | `*http://cs-g2-crl.thawte.com/ThawteCSG2.crl0` | certificado/firma distribuida; NO VERIFICADO como destino runtime |
| 0x59360 / sin RVA | ASCII | `http://ocsp.thawte.com0` | certificado/firma distribuida; NO VERIFICADO como destino runtime |
| 0x59737 / sin RVA | ASCII | `#http://crl.thawte.com/ThawtePCA.crl0` | certificado/firma distribuida; NO VERIFICADO como destino runtime |
| 0x59789 / sin RVA | ASCII | `http://ocsp.thawte.com0` | certificado/firma distribuida; NO VERIFICADO como destino runtime |
| 0x59bd1 / sin RVA | ASCII | `http://ts-ocsp.ws.symantec.com07` | certificado/firma distribuida; NO VERIFICADO como destino runtime |
| 0x59bfc / sin RVA | ASCII | `+http://ts-aia.ws.symantec.com/tss-ca-g2.cer0<` | certificado/firma distribuida; NO VERIFICADO como destino runtime |
| 0x59c3a / sin RVA | ASCII | `+http://ts-crl.ws.symantec.com/tss-ca-g2.crl0(` | certificado/firma distribuida; NO VERIFICADO como destino runtime |

### vivoxplatform.dll

Identidad SHA-256: `358fa8a00a8a704b2f6d0731884d1bbfa87a47e518f52378f2ceed9fdc2050e4`. Evidencia: `evidence/auth/vivoxplatform-pe.json`.

| Offset / RVA | Encoding | Texto literal | Interpretación/estado |
|---|---|---|---|
| 0xa5298 / 0xa6498 | ASCII | `127.0.0.1` | infraestructura/red local o ejemplo genérico; NO VERIFICADO en Hawken |
| 0xa7778 / 0xa8978 | ASCII | `198.41.0.4` | infraestructura/red local o ejemplo genérico; NO VERIFICADO en Hawken |
| 0xd5926 / sin RVA | ASCII | `http://ocsp.thawte.com0` | certificado/firma distribuida; NO VERIFICADO como destino runtime |
| 0xd5962 / sin RVA | ASCII | `.http://crl.thawte.com/ThawteTimestampingCA.crl0` | certificado/firma distribuida; NO VERIFICADO como destino runtime |
| 0xd5d0d / sin RVA | ASCII | `#http://crl.thawte.com/ThawtePCA.crl0` | certificado/firma distribuida; NO VERIFICADO como destino runtime |
| 0xd5d5f / sin RVA | ASCII | `http://ocsp.thawte.com0` | certificado/firma distribuida; NO VERIFICADO como destino runtime |
| 0xd61a7 / sin RVA | ASCII | `http://ts-ocsp.ws.symantec.com07` | certificado/firma distribuida; NO VERIFICADO como destino runtime |
| 0xd61d2 / sin RVA | ASCII | `+http://ts-aia.ws.symantec.com/tss-ca-g2.cer0<` | certificado/firma distribuida; NO VERIFICADO como destino runtime |
| 0xd6210 / sin RVA | ASCII | `+http://ts-crl.ws.symantec.com/tss-ca-g2.crl0(` | certificado/firma distribuida; NO VERIFICADO como destino runtime |
| 0xd6652 / sin RVA | ASCII | `http://th.symcb.com/th.crl0` | certificado/firma distribuida; NO VERIFICADO como destino runtime |
| 0xd66c7 / sin RVA | ASCII | `https://d.symcb.com/cps0%` | certificado/firma distribuida; NO VERIFICADO como destino runtime |
| 0xd66ee / sin RVA | ASCII | `https://d.symcb.com/rpa0` | certificado/firma distribuida; NO VERIFICADO como destino runtime |
| 0xd6742 / sin RVA | ASCII | `http://th.symcd.com0&` | certificado/firma distribuida; NO VERIFICADO como destino runtime |
| 0xd6763 / sin RVA | ASCII | `http://th.symcb.com/th.crt0` | certificado/firma distribuida; NO VERIFICADO como destino runtime |

### vivoxsdk_x64.dll

Identidad SHA-256: `a0140b99a85083308e9bf337baf7cd78f1a3e975fb4e94869d172a3cf960d9a0`. Evidencia: `evidence/auth/vivoxsdk_x64-pe.json`.

| Offset / RVA | Encoding | Texto literal | Interpretación/estado |
|---|---|---|---|
| 0x3c33f0 / 0x3c4bf0 | ASCII | `0.0.0.0` | infraestructura/red local o ejemplo genérico; NO VERIFICADO en Hawken |
| 0x3c37c8 / 0x3c4fc8 | ASCII | `127.0.0.1` | infraestructura/red local o ejemplo genérico; NO VERIFICADO en Hawken |
| 0x3c4318 / 0x3c5b18 | ASCII | `192.168.0.0` | infraestructura/red local o ejemplo genérico; NO VERIFICADO en Hawken |
| 0x3c4328 / 0x3c5b28 | ASCII | `10.0.0.0` | infraestructura/red local o ejemplo genérico; NO VERIFICADO en Hawken |
| 0x3c4338 / 0x3c5b38 | ASCII | `100.64.0.0` | infraestructura/red local o ejemplo genérico; NO VERIFICADO en Hawken |
| 0x3c4348 / 0x3c5b48 | ASCII | `172.16.0.0` | infraestructura/red local o ejemplo genérico; NO VERIFICADO en Hawken |
| 0x3c4358 / 0x3c5b58 | ASCII | `169.254.0.0` | infraestructura/red local o ejemplo genérico; NO VERIFICADO en Hawken |
| 0x3c4520 / 0x3c5d20 | ASCII | `eXosip: Cannot bind socket node: 0.0.0.0 family:%d` | infraestructura/red local o ejemplo genérico; NO VERIFICADO en Hawken |
| 0x3c4588 / 0x3c5d88 | ASCII | `198.41.0.4` | infraestructura/red local o ejemplo genérico; NO VERIFICADO en Hawken |
| 0x40b180 / 0x40c980 | ASCII | `REGISTER sip:bnw.libretel.com SIP/2.0` | fixture SIP distribuido; NO VERIFICADO en Hawken |
| 0x40b1a7 / 0x40c9a7 | ASCII | `Via: SIP/2.0/UDP 192.168.0.33:28630` | fixture SIP distribuido; NO VERIFICADO en Hawken |
| 0x40b1de / 0x40c9de | ASCII | `From: <sip:mikes@bnw.libretel.com>;tag=956de0fdb4004ef0a3f476466ef186db;epid=61d2cb5849` | fixture SIP distribuido; NO VERIFICADO en Hawken |
| 0x40b237 / 0x40ca37 | ASCII | `To: <sip:mikes@bnw.libretel.com>` | fixture SIP distribuido; NO VERIFICADO en Hawken |
| 0x40b296 / 0x40ca96 | ASCII | `Contact: <sip:192.168.0.33:28630>;methods="INVITE, MESSAGE, INFO, SUBSCRIBE, OPTIONS, BYE, CANCEL, NOTIFY, ACK, REFER, BENOTIFY"` | fixture SIP distribuido; NO VERIFICADO en Hawken |
| 0x40b3c0 / 0x40cbc0 | ASCII | `Via: SIP/2.0/UDP 192.168.0.33:28630` | fixture SIP distribuido; NO VERIFICADO en Hawken |
| 0x40b3e5 / 0x40cbe5 | ASCII | `From: <sip:mikes@bnw.libretel.com>;tag=956de0fdb4004ef0a3f476466ef186db;epid=61d2cb5849` | fixture SIP distribuido; NO VERIFICADO en Hawken |
| 0x40b43e / 0x40cc3e | ASCII | `To: <sip:mikes@bnw.libretel.com>;tag=67c5f80c43d5d9c0491b7a313ad3bef6.ae48` | fixture SIP distribuido; NO VERIFICADO en Hawken |
| 0x40b4ec / 0x40ccec | ASCII | `Contact: <sip:192.168.0.33:28630>;q=0.1;expires=1200` | fixture SIP distribuido; NO VERIFICADO en Hawken |
| 0x40b540 / 0x40cd40 | ASCII | `INVITE sip:confctl-64@bnw.libretel.com SIP/2.0` | fixture SIP distribuido; NO VERIFICADO en Hawken |
| 0x40b570 / 0x40cd70 | ASCII | `Via: SIP/2.0/UDP 192.168.0.33:28630` | fixture SIP distribuido; NO VERIFICADO en Hawken |
| 0x40b5a7 / 0x40cda7 | ASCII | `From: "MikeS" <sip:mikes@bnw.libretel.com>;tag=e5f023ce3409481588f4294f4f0fbbff;epid=61d2cb5849` | fixture SIP distribuido; NO VERIFICADO en Hawken |
| 0x40b608 / 0x40ce08 | ASCII | `To: <sip:confctl-64@bnw.libretel.com>` | fixture SIP distribuido; NO VERIFICADO en Hawken |
| 0x40b66a / 0x40ce6a | ASCII | `Contact: <sip:192.168.0.33:28630>` | fixture SIP distribuido; NO VERIFICADO en Hawken |
| 0x40b6dd / 0x40cedd | ASCII | `o=- 0 0 IN IP4 192.168.0.33` | fixture SIP distribuido; NO VERIFICADO en Hawken |
| 0x40b705 / 0x40cf05 | ASCII | `c=IN IP4 192.168.0.33` | fixture SIP distribuido; NO VERIFICADO en Hawken |
| 0x40b910 / 0x40d110 | ASCII | `Via: SIP/2.0/UDP 192.168.0.33:28630` | fixture SIP distribuido; NO VERIFICADO en Hawken |
| 0x40b970 / 0x40d170 | ASCII | `From: "MikeS" <sip:mikes@bnw.libretel.com>;tag=e5f023ce3409481588f4294f4f0fbbff;epid=61d2cb5849` | fixture SIP distribuido; NO VERIFICADO en Hawken |
| 0x40b9d1 / 0x40d1d1 | ASCII | `To: <sip:confctl-64@bnw.libretel.com>;tag=1116964531725433868` | fixture SIP distribuido; NO VERIFICADO en Hawken |
| 0x40ba10 / 0x40d210 | ASCII | `Contact: sip:69.90.168.6:5060` | fixture SIP distribuido; NO VERIFICADO en Hawken |
| 0x40ba2f / 0x40d22f | ASCII | `Record-Route: <sip:69.90.155.77;ftag=e5f023ce3409481588f4294f4f0fbbff;lr=on> ` | fixture SIP distribuido; NO VERIFICADO en Hawken |
| 0x40bb0b / 0x40d30b | ASCII | `o=edial_ivr 1116964531725433868 1725433870 IN IP4 69.90.168.6` | fixture SIP distribuido; NO VERIFICADO en Hawken |
| 0x40bb58 / 0x40d358 | ASCII | `c=IN IP4 69.90.168.6` | fixture SIP distribuido; NO VERIFICADO en Hawken |
| 0x40c610 / 0x40de10 | ASCII | `, a.root-servers.net` | tercero/literal; CONFIRMADO existencia; uso runtime NO VERIFICADO |
| 0x40c628 / 0x40de28 | ASCII | `a.root-servers.net` | tercero/literal; CONFIRMADO existencia; uso runtime NO VERIFICADO |
| 0x429bb7 / 0x42b3b7 | ASCII | `xmlns:SOAP-ENV="http://schemas.xmlsoap.org/soap/envelope/" xmlns:xsd="http://www.w3.org/2001/XMLSchema" ` | namespace/documentación; NO VERIFICADO como destino runtime |
| 0x429c23 / 0x42b423 | ASCII | `xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance"` | namespace/documentación; NO VERIFICADO como destino runtime |
| 0x429c5b / 0x42b45b | ASCII | `xmlns:SOAP-ENC="http://schemas.xmlsoap.org/soap/encoding/" ` | namespace/documentación; NO VERIFICADO como destino runtime |
| 0x429cc0 / 0x42b4c0 | ASCII | `xmlns:ns5="http://soapinterop.org/xsd" ` | namespace/documentación; NO VERIFICADO como destino runtime |
| 0x429cea / 0x42b4ea | ASCII | `SOAP-ENV:encodingStyle="http://schemas.xmlsoap.org/soap/encoding/">` | namespace/documentación; NO VERIFICADO como destino runtime |
| 0x429f45 / 0x42b745 | ASCII | `xmlns:SOAP-ENV="http://schemas.xmlsoap.org/soap/envelope/" xmlns:xsd="http://www.w3.org/2001/XMLSchema" ` | namespace/documentación; NO VERIFICADO como destino runtime |
| 0x429faf / 0x42b7af | ASCII | `xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance"` | namespace/documentación; NO VERIFICADO como destino runtime |
| 0x429fe5 / 0x42b7e5 | ASCII | `xmlns:SOAP-ENC="http://schemas.xmlsoap.org/soap/encoding/" ` | namespace/documentación; NO VERIFICADO como destino runtime |
| 0x42a046 / 0x42b846 | ASCII | `xmlns:ns5="http://soapinterop.org/xsd" ` | namespace/documentación; NO VERIFICADO como destino runtime |
| 0x42a06e / 0x42b86e | ASCII | `SOAP-ENV:encodingStyle="http://schemas.xmlsoap.org/soap/encoding/">` | namespace/documentación; NO VERIFICADO como destino runtime |
| 0x42a235 / 0x42ba35 | ASCII | `xmlns:SOAP-ENV="http://schemas.xmlsoap.org/soap/envelope/" xmlns:xsd="http://www.w3.org/2001/XMLSchema" ` | namespace/documentación; NO VERIFICADO como destino runtime |
| 0x42a29f / 0x42ba9f | ASCII | `xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance"` | namespace/documentación; NO VERIFICADO como destino runtime |
| 0x42a2d5 / 0x42bad5 | ASCII | `xmlns:SOAP-ENC="http://schemas.xmlsoap.org/soap/encoding/" ` | namespace/documentación; NO VERIFICADO como destino runtime |
| 0x42a336 / 0x42bb36 | ASCII | `xmlns:ns5="http://soapinterop.org/xsd" ` | namespace/documentación; NO VERIFICADO como destino runtime |
| 0x42a35e / 0x42bb5e | ASCII | `SOAP-ENV:encodingStyle="http://schemas.xmlsoap.org/soap/encoding/">` | namespace/documentación; NO VERIFICADO como destino runtime |
| 0x42d578 / 0x42ed78 | ASCII | `https://watson.vivox.com` | tercero/literal; CONFIRMADO existencia; uso runtime NO VERIFICADO |
| 0x5323a2 / 0x5345a2 | ASCII | `0.0.0.0` | infraestructura/red local o ejemplo genérico; NO VERIFICADO en Hawken |
| 0x532432 / 0x534632 | ASCII | `0.0.0.0` | infraestructura/red local o ejemplo genérico; NO VERIFICADO en Hawken |
| 0x5a0526 / sin RVA | ASCII | `http://ocsp.thawte.com0` | certificado/firma distribuida; NO VERIFICADO como destino runtime |
| 0x5a0562 / sin RVA | ASCII | `.http://crl.thawte.com/ThawteTimestampingCA.crl0` | certificado/firma distribuida; NO VERIFICADO como destino runtime |
| 0x5a090d / sin RVA | ASCII | `#http://crl.thawte.com/ThawtePCA.crl0` | certificado/firma distribuida; NO VERIFICADO como destino runtime |
| 0x5a095f / sin RVA | ASCII | `http://ocsp.thawte.com0` | certificado/firma distribuida; NO VERIFICADO como destino runtime |
| 0x5a0da7 / sin RVA | ASCII | `http://ts-ocsp.ws.symantec.com07` | certificado/firma distribuida; NO VERIFICADO como destino runtime |
| 0x5a0dd2 / sin RVA | ASCII | `+http://ts-aia.ws.symantec.com/tss-ca-g2.cer0<` | certificado/firma distribuida; NO VERIFICADO como destino runtime |
| 0x5a0e10 / sin RVA | ASCII | `+http://ts-crl.ws.symantec.com/tss-ca-g2.crl0(` | certificado/firma distribuida; NO VERIFICADO como destino runtime |
| 0x5a1252 / sin RVA | ASCII | `http://th.symcb.com/th.crl0` | certificado/firma distribuida; NO VERIFICADO como destino runtime |
| 0x5a12c7 / sin RVA | ASCII | `https://d.symcb.com/cps0%` | certificado/firma distribuida; NO VERIFICADO como destino runtime |
| 0x5a12ee / sin RVA | ASCII | `https://d.symcb.com/rpa0` | certificado/firma distribuida; NO VERIFICADO como destino runtime |
| 0x5a1342 / sin RVA | ASCII | `http://th.symcd.com0&` | certificado/firma distribuida; NO VERIFICADO como destino runtime |
| 0x5a1363 / sin RVA | ASCII | `http://th.symcb.com/th.crt0` | certificado/firma distribuida; NO VERIFICADO como destino runtime |

## Candidatos IPv6 y tokens cortos

Un recorrido adicional valida tokens IPv6 con `ipaddress.IPv6Address`, incluyendo strings ASCII cortas de 2-3 caracteres. `2001:503:ba3e::2:30` en Vivox SDK (offset `0x3c4598`) está cerca de `198.41.0.4` y es PROBABLE infraestructura de resolución genérica. Los tokens `::`, `::0`, `::1`, `::2`, `::8` pueden ser direcciones neutras o coincidencias en código/datos: se retienen como candidatos y no se atribuyen a backend. Evidencia: `ipv6-candidates.json`; cada fila conserva hash y texto circundante. CONFIRMADO token; uso como endpoint Hawken NO VERIFICADO.

| Archivo | Token validado | Offsets de string |
|---|---|---|
| `libeay32.dll` | `::` | 0xe8f8e, 0xe9844 |
| `ortp.dll` | `::` | 0x38fa4 |
| `ortp_x64.dll` | `::` | 0x39948 |
| `steam_api64.dll` | `::` | 0x36a54 |
| `vivoxplatform.dll` | `::` | 0xa3234 |
| `vivoxsdk_x64.dll` | `2001:503:ba3e::2:30` | 0x3c4598 |
| `vivoxsdk_x64.dll` | `::` | 0x3c13b4, 0x4e5625, 0x4e562d, 0x4e5635, 0x4e5ab1, 0x4e5ab8, 0x4e5ac0, 0x4e5ae9, 0x4e5af1, 0x4e5af9, 0x4e5b01, 0x4e5ee9, 0x4e623d, 0x4e6271, 0x4e64dd, 0x4e6511, 0x541c1d, 0x541ebb |
| `vivoxsdk_x64.dll` | `::0` | 0x3c4580 |
| `vivoxsdk_x64.dll` | `::1` | 0x3c5b44 |
| `vivoxsdk_x64.dll` | `::2` | 0xa5fc2 |
| `vivoxsdk_x64.dll` | `::8` | 0x49be44 |

## Límites de cobertura y próximos datos necesarios

La extracción cubre strings imprimibles ASCII y UTF16LE de mínimo cuatro caracteres en los once artefactos identificados, más texto de certificados DER cuando está en WIN_CERTIFICATE y un recorrido IPv6 separado. Detecta URLs por esquemas HTTP/HTTPS/WS/WSS/FTP/SIP/STUN/TURN con `://`, dominios con TLDs enumerados en el script e IPv4 validada por octetos. Fragmentos SIP y rutas se recorrieron aparte en Meteor. No equivale a un parser completo de IDN ni prueba ausencia de direcciones cifradas, comprimidas o construidas de otra forma.

Las xrefs automáticas cubren operandos RIP visibles en objdump hacia los comienzos de strings del área personalizada `RVA 0x1a0000..0x1aa000`. No abarcan punteros indirectos, tablas completas, código generado ni todos los flujos de los terceros. `.pdata` da límites de funciones admitidas, no nombres originales. Faltan schemas completos de APIs y validación de helpers de serialización; ningún protocolo se considera reconstruido de extremo a extremo.

Los helpers de auth/base64/URI y los parsers principales REST ya se corroboraron con cinco lotes Ghidra (55 funciones). Sigue pendiente el flujo efectivo del EXE que selecciona consumidores/estados y el significado de grants/SessionKey que debe validar el servidor; rutas de inventario/party/clan/offers conservan principalmente evidencia de literales y xrefs. Validar el comportamiento de un servidor emulado requeriría un alcance posterior autorizado y evidencia de ejecución aislada. Este trabajo no modifica ni ejecuta el cliente ni construye el servidor.

## Endpoints literales del EXE principal aportados por el segundo escritor

Archivo: `F:\SteamLibrary\steamapps\common\Hawken\Binaries\Win64\HawkenGame-Win64-Shipping.exe`; SHA-256 `4a861ecb4ac7add9554f8daf8f98ad2c12b9cb6c1865a83e8be808beb46ea8a2`; ImageBase `0x140000000`. Fuente `evidence/game/exe-endpoints.json`, cotejo de bytes `evidence/auth/exe-endpoints-reviewed.json`: 106 candidatos ASCII/UTF16 con offsets originales. El cotejo conserva el casing original y registra dos normalizaciones del esquema/host del scanner (offsets `0x177a5c0` y `0x177a988`); los literales de la tabla usan los bytes originales. Todos son CONFIRMADOS como bytes; su uso runtime es NO VERIFICADO. Candidatos no clasificados incluyen textos que no son hosts operativos. La evidencia completa de xrefs candidatas está en `game/exe-endpoint-xref-candidates.json`; el literal live PC tiene LEA candidata RVA `0xbc3614` dentro de función `0xbc3510..0xbc46c8`, sin afirmar aún selección runtime.

| Literal/candidato | Offset / RVA | Encoding y categoría del extractor | Estado/interpretación |
|---|---|---|---|
| `http://www.w3.org/XML/1998/namespace` | 0x180c0b4 / 0x180ccb4 | ascii · NO_CLASIFICADO | CONFIRMADO literal; namespace stock XML. |
| `www.w3.org` | 0x180c0bb / 0x180ccbb | ascii · NO_CLASIFICADO | CONFIRMADO literal; namespace stock XML. |
| `http://www.w3.org/XML/1998/namespace` | 0x180c0e0 / 0x180cce0 | ascii · NO_CLASIFICADO | CONFIRMADO literal; namespace stock XML. |
| `www.w3.org` | 0x180c0e7 / 0x180cce7 | ascii · NO_CLASIFICADO | CONFIRMADO literal; namespace stock XML. |
| `http://www.w3.org/2000/xmlns/` | 0x180c110 / 0x180cd10 | ascii · NO_CLASIFICADO | CONFIRMADO literal; namespace stock XML. |
| `www.w3.org` | 0x180c117 / 0x180cd17 | ascii · NO_CLASIFICADO | CONFIRMADO literal; namespace stock XML. |
| `6.0.0.0` | 0x1ca6cf2 / 0x1e534f2 | ascii · NO_CLASIFICADO | CONFIRMADO literal; texto/candidato de tercero; no endpoint operativo demostrado. |
| `http://www.unrealtournament3.com/` | 0x12d73e0 / 0x12d7fe0 | utf-16le · REFERENCIA_UE3_STOCK_CANDIDATA | CONFIRMADO literal; sitio stock UE3. |
| `www.unrealtournament3.com` | 0x12d73ee / 0x12d7fee | utf-16le · REFERENCIA_UE3_STOCK_CANDIDATA | CONFIRMADO literal; sitio stock UE3. |
| `172.23.2.8` | 0x1414b58 / 0x1415758 | utf-16le · NO_CLASIFICADO | CONFIRMADO literal; texto/candidato de tercero; no endpoint operativo demostrado. |
| `http://www.nvidia.com/object/physx_system_software.html` | 0x14f3274 / 0x14f3e74 | utf-16le · NO_CLASIFICADO | CONFIRMADO literal; texto/candidato de tercero; no endpoint operativo demostrado. |
| `www.nvidia.com` | 0x14f3282 / 0x14f3e82 | utf-16le · NO_CLASIFICADO | CONFIRMADO literal; texto/candidato de tercero; no endpoint operativo demostrado. |
| `127.0.0.1` | 0x1504dc2 / 0x15059c2 | utf-16le · DIRECCION_LOCAL_O_WILDCARD | CONFIRMADO literal; texto/candidato de tercero; no endpoint operativo demostrado. |
| `127.0.0.1` | 0x1504eda / 0x1505ada | utf-16le · DIRECCION_LOCAL_O_WILDCARD | CONFIRMADO literal; texto/candidato de tercero; no endpoint operativo demostrado. |
| `sip:confctl-g-` | 0x16abd38 / 0x16ac938 | utf-16le · NO_CLASIFICADO | CONFIRMADO literal; texto/candidato de tercero; no endpoint operativo demostrado. |
| `https://v2-services-staged-pc.playhawken.com` | 0x16afcc0 / 0x16b08c0 | utf-16le · PERSONALIZADO_O_INTEGRACION_CANDIDATA | CONFIRMADO literal; literal personalizado Hawken; uso pendiente. |
| `v2-services-staged-pc.playhawken.com` | 0x16afcd0 / 0x16b08d0 | utf-16le · PERSONALIZADO_O_INTEGRACION_CANDIDATA | CONFIRMADO literal; literal personalizado Hawken; uso pendiente. |
| `https://v2-services-staged-xb1.playhawken.com` | 0x16afee0 / 0x16b0ae0 | utf-16le · PERSONALIZADO_O_INTEGRACION_CANDIDATA | CONFIRMADO literal; literal personalizado Hawken; uso pendiente. |
| `v2-services-staged-xb1.playhawken.com` | 0x16afef0 / 0x16b0af0 | utf-16le · PERSONALIZADO_O_INTEGRACION_CANDIDATA | CONFIRMADO literal; literal personalizado Hawken; uso pendiente. |
| `https://v2-services-staged-ps4.playhawken.com` | 0x16aff40 / 0x16b0b40 | utf-16le · PERSONALIZADO_O_INTEGRACION_CANDIDATA | CONFIRMADO literal; literal personalizado Hawken; uso pendiente. |
| `v2-services-staged-ps4.playhawken.com` | 0x16aff50 / 0x16b0b50 | utf-16le · PERSONALIZADO_O_INTEGRACION_CANDIDATA | CONFIRMADO literal; literal personalizado Hawken; uso pendiente. |
| `https://v2-services-main-pc.playhawken.com` | 0x16affa0 / 0x16b0ba0 | utf-16le · PERSONALIZADO_O_INTEGRACION_CANDIDATA | CONFIRMADO literal; literal personalizado Hawken; uso pendiente. |
| `v2-services-main-pc.playhawken.com` | 0x16affb0 / 0x16b0bb0 | utf-16le · PERSONALIZADO_O_INTEGRACION_CANDIDATA | CONFIRMADO literal; literal personalizado Hawken; uso pendiente. |
| `https://v2-services-main-xb1.playhawken.com` | 0x16b0010 / 0x16b0c10 | utf-16le · PERSONALIZADO_O_INTEGRACION_CANDIDATA | CONFIRMADO literal; literal personalizado Hawken; uso pendiente. |
| `v2-services-main-xb1.playhawken.com` | 0x16b0020 / 0x16b0c20 | utf-16le · PERSONALIZADO_O_INTEGRACION_CANDIDATA | CONFIRMADO literal; literal personalizado Hawken; uso pendiente. |
| `https://v2-services-main-ps4.playhawken.com` | 0x16b0090 / 0x16b0c90 | utf-16le · PERSONALIZADO_O_INTEGRACION_CANDIDATA | CONFIRMADO literal; literal personalizado Hawken; uso pendiente. |
| `v2-services-main-ps4.playhawken.com` | 0x16b00a0 / 0x16b0ca0 | utf-16le · PERSONALIZADO_O_INTEGRACION_CANDIDATA | CONFIRMADO literal; literal personalizado Hawken; uso pendiente. |
| `https://v2-services-live-pc.playhawken.com` | 0x16b0120 / 0x16b0d20 | utf-16le · PERSONALIZADO_O_INTEGRACION_CANDIDATA | CONFIRMADO literal; literal personalizado Hawken; uso pendiente. |
| `v2-services-live-pc.playhawken.com` | 0x16b0130 / 0x16b0d30 | utf-16le · PERSONALIZADO_O_INTEGRACION_CANDIDATA | CONFIRMADO literal; literal personalizado Hawken; uso pendiente. |
| `https://v2-services-live-xb1-2.playhawken.com` | 0x16b01d0 / 0x16b0dd0 | utf-16le · PERSONALIZADO_O_INTEGRACION_CANDIDATA | CONFIRMADO literal; literal personalizado Hawken; uso pendiente. |
| `v2-services-live-xb1-2.playhawken.com` | 0x16b01e0 / 0x16b0de0 | utf-16le · PERSONALIZADO_O_INTEGRACION_CANDIDATA | CONFIRMADO literal; literal personalizado Hawken; uso pendiente. |
| `https://v2-services-live-ps4-2.playhawken.com` | 0x16b0230 / 0x16b0e30 | utf-16le · PERSONALIZADO_O_INTEGRACION_CANDIDATA | CONFIRMADO literal; literal personalizado Hawken; uso pendiente. |
| `v2-services-live-ps4-2.playhawken.com` | 0x16b0240 / 0x16b0e40 | utf-16le · PERSONALIZADO_O_INTEGRACION_CANDIDATA | CONFIRMADO literal; literal personalizado Hawken; uso pendiente. |
| `http://www.google.com` | 0x1702f98 / 0x1703b98 | utf-16le · NO_CLASIFICADO | CONFIRMADO literal; texto/candidato de tercero; no endpoint operativo demostrado. |
| `www.google.com` | 0x1702fa6 / 0x1703ba6 | utf-16le · NO_CLASIFICADO | CONFIRMADO literal; texto/candidato de tercero; no endpoint operativo demostrado. |
| `www.google.com` | 0x1703016 / 0x1703c16 | utf-16le · NO_CLASIFICADO | CONFIRMADO literal; texto/candidato de tercero; no endpoint operativo demostrado. |
| `http://files.aoaforums.com/I3199-setup.zip.html` | 0x170bbda / 0x170c7da | utf-16le · NO_CLASIFICADO | CONFIRMADO literal; texto/candidato de tercero; no endpoint operativo demostrado. |
| `files.aoaforums.com` | 0x170bbe8 / 0x170c7e8 | utf-16le · NO_CLASIFICADO | CONFIRMADO literal; texto/candidato de tercero; no endpoint operativo demostrado. |
| `http://files.aoaforums.com/I3199-setup.zip.html` | 0x170bd2a / 0x170c92a | utf-16le · NO_CLASIFICADO | CONFIRMADO literal; texto/candidato de tercero; no endpoint operativo demostrado. |
| `files.aoaforums.com` | 0x170bd38 / 0x170c938 | utf-16le · NO_CLASIFICADO | CONFIRMADO literal; texto/candidato de tercero; no endpoint operativo demostrado. |
| `http://files.aoaforums.com/I3199-setup.zip.html` | 0x170bf0a / 0x170cb0a | utf-16le · NO_CLASIFICADO | CONFIRMADO literal; texto/candidato de tercero; no endpoint operativo demostrado. |
| `files.aoaforums.com` | 0x170bf18 / 0x170cb18 | utf-16le · NO_CLASIFICADO | CONFIRMADO literal; texto/candidato de tercero; no endpoint operativo demostrado. |
| `http://files.aoaforums.com/I3199-setup.zip.html` | 0x170c06a / 0x170cc6a | utf-16le · NO_CLASIFICADO | CONFIRMADO literal; texto/candidato de tercero; no endpoint operativo demostrado. |
| `files.aoaforums.com` | 0x170c078 / 0x170cc78 | utf-16le · NO_CLASIFICADO | CONFIRMADO literal; texto/candidato de tercero; no endpoint operativo demostrado. |
| `127.0.0.1` | 0x170f948 / 0x1710548 | utf-16le · DIRECCION_LOCAL_O_WILDCARD | CONFIRMADO literal; texto/candidato de tercero; no endpoint operativo demostrado. |
| `33q.CN` | 0x176b798 / 0x176c398 | utf-16le · NO_CLASIFICADO | CONFIRMADO literal; texto/candidato de tercero; no endpoint operativo demostrado. |
| `3803143.MOZONE.CN` | 0x176baa0 / 0x176c6a0 | utf-16le · NO_CLASIFICADO | CONFIRMADO literal; texto/candidato de tercero; no endpoint operativo demostrado. |
| `3g.99kp.net` | 0x176c030 / 0x176cc30 | utf-16le · NO_CLASIFICADO | CONFIRMADO literal; texto/candidato de tercero; no endpoint operativo demostrado. |
| `3jpp.Cn` | 0x176c2d8 / 0x176ced8 | utf-16le · NO_CLASIFICADO | CONFIRMADO literal; texto/candidato de tercero; no endpoint operativo demostrado. |
| `3paa.cn` | 0x176c668 / 0x176d268 | utf-16le · NO_CLASIFICADO | CONFIRMADO literal; texto/candidato de tercero; no endpoint operativo demostrado. |
| `51top.ned.cn` | 0x176cf38 / 0x176db38 | utf-16le · NO_CLASIFICADO | CONFIRMADO literal; texto/candidato de tercero; no endpoint operativo demostrado. |
| `59N.CN` | 0x176d308 / 0x176df08 | utf-16le · NO_CLASIFICADO | CONFIRMADO literal; texto/candidato de tercero; no endpoint operativo demostrado. |
| `66mm.org` | 0x176d8b8 / 0x176e4b8 | utf-16le · NO_CLASIFICADO | CONFIRMADO literal; texto/candidato de tercero; no endpoint operativo demostrado. |
| `66mm.org` | 0x176d9e0 / 0x176e5e0 | utf-16le · NO_CLASIFICADO | CONFIRMADO literal; texto/candidato de tercero; no endpoint operativo demostrado. |
| `66mm.org` | 0x176db48 / 0x176e748 | utf-16le · NO_CLASIFICADO | CONFIRMADO literal; texto/candidato de tercero; no endpoint operativo demostrado. |
| `66mm.org` | 0x176dbe8 / 0x176e7e8 | utf-16le · NO_CLASIFICADO | CONFIRMADO literal; texto/candidato de tercero; no endpoint operativo demostrado. |
| `66mm.org` | 0x176dce0 / 0x176e8e0 | utf-16le · NO_CLASIFICADO | CONFIRMADO literal; texto/candidato de tercero; no endpoint operativo demostrado. |
| `88821.CN` | 0x176e3f0 / 0x176eff0 | utf-16le · NO_CLASIFICADO | CONFIRMADO literal; texto/candidato de tercero; no endpoint operativo demostrado. |
| `9ise.org` | 0x176f838 / 0x1770438 | utf-16le · NO_CLASIFICADO | CONFIRMADO literal; texto/candidato de tercero; no endpoint operativo demostrado. |
| `aphh.com` | 0x1770548 / 0x1771148 | utf-16le · NO_CLASIFICADO | CONFIRMADO literal; texto/candidato de tercero; no endpoint operativo demostrado. |
| `GAAE.CN` | 0x1778990 / 0x1779590 | utf-16le · NO_CLASIFICADO | CONFIRMADO literal; texto/candidato de tercero; no endpoint operativo demostrado. |
| `Gx123.ddbar.com` | 0x17790a0 / 0x1779ca0 | utf-16le · NO_CLASIFICADO | CONFIRMADO literal; texto/candidato de tercero; no endpoint operativo demostrado. |
| `gz.pkwap.com` | 0x1779298 / 0x1779e98 | utf-16le · NO_CLASIFICADO | CONFIRMADO literal; texto/candidato de tercero; no endpoint operativo demostrado. |
| `http://163xg.com/bbs` | 0x177a260 / 0x177ae60 | utf-16le · NO_CLASIFICADO | CONFIRMADO literal; texto/candidato de tercero; no endpoint operativo demostrado. |
| `163xg.com` | 0x177a26e / 0x177ae6e | utf-16le · NO_CLASIFICADO | CONFIRMADO literal; texto/candidato de tercero; no endpoint operativo demostrado. |
| `http://511596890.spaces.live.com` | 0x177a3e0 / 0x177afe0 | utf-16le · NO_CLASIFICADO | CONFIRMADO literal; texto/candidato de tercero; no endpoint operativo demostrado. |
| `511596890.spaces.live.com` | 0x177a3ee / 0x177afee | utf-16le · NO_CLASIFICADO | CONFIRMADO literal; texto/candidato de tercero; no endpoint operativo demostrado. |
| `http://66mm.org/81140` | 0x177a540 / 0x177b140 | utf-16le · NO_CLASIFICADO | CONFIRMADO literal; texto/candidato de tercero; no endpoint operativo demostrado. |
| `66mm.org` | 0x177a54e / 0x177b14e | utf-16le · NO_CLASIFICADO | CONFIRMADO literal; texto/candidato de tercero; no endpoint operativo demostrado. |
| `HTTP://66mm.org/83727` | 0x177a5c0 / 0x177b1c0 | utf-16le · NO_CLASIFICADO | CONFIRMADO literal; texto/candidato de tercero; no endpoint operativo demostrado. |
| `66mm.org` | 0x177a5ce / 0x177b1ce | utf-16le · NO_CLASIFICADO | CONFIRMADO literal; texto/candidato de tercero; no endpoint operativo demostrado. |
| `http://66mm.org/95086` | 0x177a718 / 0x177b318 | utf-16le · NO_CLASIFICADO | CONFIRMADO literal; texto/candidato de tercero; no endpoint operativo demostrado. |
| `66mm.org` | 0x177a726 / 0x177b326 | utf-16le · NO_CLASIFICADO | CONFIRMADO literal; texto/candidato de tercero; no endpoint operativo demostrado. |
| `http://Q19.waps.cn` | 0x177a988 / 0x177b588 | utf-16le · NO_CLASIFICADO | CONFIRMADO literal; texto/candidato de tercero; no endpoint operativo demostrado. |
| `Q19.waps.cn` | 0x177a996 / 0x177b596 | utf-16le · NO_CLASIFICADO | CONFIRMADO literal; texto/candidato de tercero; no endpoint operativo demostrado. |
| `hypermart.net` | 0x177ae90 / 0x177ba90 | utf-16le · NO_CLASIFICADO | CONFIRMADO literal; texto/candidato de tercero; no endpoint operativo demostrado. |
| `kkwap.net` | 0x177c460 / 0x177d060 | utf-16le · NO_CLASIFICADO | CONFIRMADO literal; texto/candidato de tercero; no endpoint operativo demostrado. |
| `konghd.cn` | 0x177c950 / 0x177d550 | utf-16le · NO_CLASIFICADO | CONFIRMADO literal; texto/candidato de tercero; no endpoint operativo demostrado. |
| `lxyes.com` | 0x177d478 / 0x177e078 | utf-16le · NO_CLASIFICADO | CONFIRMADO literal; texto/candidato de tercero; no endpoint operativo demostrado. |
| `mm.org` | 0x177e688 / 0x177f288 | utf-16le · NO_CLASIFICADO | CONFIRMADO literal; texto/candidato de tercero; no endpoint operativo demostrado. |
| `oopt.yywap.com` | 0x1780830 / 0x1781430 | utf-16le · NO_CLASIFICADO | CONFIRMADO literal; texto/candidato de tercero; no endpoint operativo demostrado. |
| `rmmm.wen.ru` | 0x17854e0 / 0x17860e0 | utf-16le · NO_CLASIFICADO | CONFIRMADO literal; texto/candidato de tercero; no endpoint operativo demostrado. |
| `s.mq520.cn` | 0x17855e0 / 0x17861e0 | utf-16le · NO_CLASIFICADO | CONFIRMADO literal; texto/candidato de tercero; no endpoint operativo demostrado. |
| `sjzm.net` | 0x1786b28 / 0x1787728 | utf-16le · NO_CLASIFICADO | CONFIRMADO literal; texto/candidato de tercero; no endpoint operativo demostrado. |
| `timewe.net` | 0x1789420 / 0x178a020 | utf-16le · NO_CLASIFICADO | CONFIRMADO literal; texto/candidato de tercero; no endpoint operativo demostrado. |
| `tpyes.com` | 0x1789b98 / 0x178a798 | utf-16le · NO_CLASIFICADO | CONFIRMADO literal; texto/candidato de tercero; no endpoint operativo demostrado. |
| `ttsku.cn` | 0x1789ed0 / 0x178aad0 | utf-16le · NO_CLASIFICADO | CONFIRMADO literal; texto/candidato de tercero; no endpoint operativo demostrado. |
| `txmf.waps.cn` | 0x178a348 / 0x178af48 | utf-16le · NO_CLASIFICADO | CONFIRMADO literal; texto/candidato de tercero; no endpoint operativo demostrado. |
| `wap.9fg.cn` | 0x178bb70 / 0x178c770 | utf-16le · NO_CLASIFICADO | CONFIRMADO literal; texto/candidato de tercero; no endpoint operativo demostrado. |
| `wap.hhhwap.cn` | 0x178bce0 / 0x178c8e0 | utf-16le · NO_CLASIFICADO | CONFIRMADO literal; texto/candidato de tercero; no endpoint operativo demostrado. |
| `wap.hhtpa.cn` | 0x178be08 / 0x178ca08 | utf-16le · NO_CLASIFICADO | CONFIRMADO literal; texto/candidato de tercero; no endpoint operativo demostrado. |
| `wap.hhtpi.cn` | 0x178bfc8 / 0x178cbc8 | utf-16le · NO_CLASIFICADO | CONFIRMADO literal; texto/candidato de tercero; no endpoint operativo demostrado. |
| `wap.mq520.Cn` | 0x178c0a8 / 0x178cca8 | utf-16le · NO_CLASIFICADO | CONFIRMADO literal; texto/candidato de tercero; no endpoint operativo demostrado. |
| `wap.wuha.cn` | 0x178c140 / 0x178cd40 | utf-16le · NO_CLASIFICADO | CONFIRMADO literal; texto/candidato de tercero; no endpoint operativo demostrado. |
| `xx.Net` | 0x178d2b8 / 0x178deb8 | utf-16le · NO_CLASIFICADO | CONFIRMADO literal; texto/candidato de tercero; no endpoint operativo demostrado. |
| `zjja.waps.cn` | 0x178dda8 / 0x178e9a8 | utf-16le · NO_CLASIFICADO | CONFIRMADO literal; texto/candidato de tercero; no endpoint operativo demostrado. |
| `cbi.loong3d.com` | 0x178f898 / 0x1790498 | utf-16le · NO_CLASIFICADO | CONFIRMADO literal; texto/candidato de tercero; no endpoint operativo demostrado. |
| `popsoft.loong3d.com` | 0x178f950 / 0x1790550 | utf-16le · NO_CLASIFICADO | CONFIRMADO literal; texto/candidato de tercero; no endpoint operativo demostrado. |
| `ppw.loong3d.com` | 0x178fab8 / 0x17906b8 | utf-16le · NO_CLASIFICADO | CONFIRMADO literal; texto/candidato de tercero; no endpoint operativo demostrado. |
| `pcgg.loong3d.com` | 0x178fb50 / 0x1790750 | utf-16le · NO_CLASIFICADO | CONFIRMADO literal; texto/candidato de tercero; no endpoint operativo demostrado. |
| `Joyplay.loong3d.com` | 0x178fc68 / 0x1790868 | utf-16le · NO_CLASIFICADO | CONFIRMADO literal; texto/candidato de tercero; no endpoint operativo demostrado. |
| `qh.loong3d.com` | 0x178fd68 / 0x1790968 | utf-16le · NO_CLASIFICADO | CONFIRMADO literal; texto/candidato de tercero; no endpoint operativo demostrado. |
| `21wuxia.loong3d.com` | 0x178fe10 / 0x1790a10 | utf-16le · NO_CLASIFICADO | CONFIRMADO literal; texto/candidato de tercero; no endpoint operativo demostrado. |
| `smsd.loong3d.com` | 0x178ff00 / 0x1790b00 | utf-16le · NO_CLASIFICADO | CONFIRMADO literal; texto/candidato de tercero; no endpoint operativo demostrado. |
| `netfriends.loong3d.com` | 0x1790000 / 0x1790c00 | utf-16le · NO_CLASIFICADO | CONFIRMADO literal; texto/candidato de tercero; no endpoint operativo demostrado. |
| `ucg.loong3d.com` | 0x17900f8 / 0x1790cf8 | utf-16le · NO_CLASIFICADO | CONFIRMADO literal; texto/candidato de tercero; no endpoint operativo demostrado. |

## Contratos REST recuperados por Ghidra

Esta sección usa el mismo SHA Meteor e ImageBase de la tabla principal. Las direcciones son RVA; offsets de archivo de código son RVA menos `0xc00` en su sección `.text`. Fuentes: `evidence/ghidra-sdk-services.jsonl`, `ghidra-sdk-schemas.jsonl`, `ghidra-sdk-results.jsonl` y el desensamblado; las keys y tipos se documentan como contrato estático del consumidor. No hay evidencia runtime ni schema completo de servidor.

| Operación | Constructor / ejecutor RVA | Request observado | Result interno observado | Estado y límites |
|---|---|---|---|---|
| Listing individual | `0x7de00` / `0x7d580` | GET `/gameServerListings/{id transformado con 0x6fd50}` | objeto procesado por `GameServerListing::CreateFromJson` `0x7a610` | CONFIRMADO ruta/helper GET y llamada parser; keys/tipos en la tabla de schemas siguiente. |
| Filtrar listings | `0x7f0d0` / `0x7e7e0` | GET `/gameServerListings` con filtros `Region=`, `GameVersion=`, `Map=`, `GameType=`, `ServerName=` | array; cada elemento usa parser `0x7a610` | CONFIRMADO casing de query y procesamiento de array. Omite filtros vacíos; aplica un escape `0x6fd50` a cada valor. Separador inicial `?` RVA `0x1a66d0` y posterior `&` RVA `0x1a66ec`; `Map=` RVA `0x1a66f8`. Helper `0x86330` prepend/concat del separador, evidencia `auth/helper-0x86330.txt`. |
| Servidores de usuario | `0x806b0` / `0x7fee0` | GET `/userGameServers/{userId}` | array (tag 6); se añaden entradas string (tag 4) a MeteorIdCollection | CONFIRMADO IDs, no listados completos. Las entradas no-string se omiten en el bucle observado. |
| Crear advertisement cliente | `0x80e80` / `0x4eaa0` | POST `/hawkenClientMatchmakingAdvertisements`; objeto serializado por `0x847d0` | JSON string (tag 4), copiada a buffer de 0x400 bytes | CONFIRMADO POST vía `0x6e900` (xref `0x6e9ca`, call `0x6e9d7`), serializer y respuesta escalar. `0x80b90` es constructor de task, no ejecutor. |
| Consultar advertisement | `0x822e0` / `0x81dc0` | GET `/hawkenClientMatchmakingAdvertisements/{id}` | objeto ClientMatchmakingAdvertisement por `0x83fb0` | CONFIRMADO helper GET/modelo; schema confirmado en parser inferior `0x84150`. |
| Cancelar advertisement | `0x82d40` / `0x82b60` | DELETE `/hawkenClientMatchmakingAdvertisements/{id}` | ApiResult<void>, sin parsing de payload interno | CONFIRMADO helper `0x6eb30` usa literal DELETE RVA `0x1a5ec0`, xref `0x6ebe8`. |
| Sesión cliente | `0x83b30` / `0x83660` | GET `/session` | objeto ClientSession parseado por `0x84da0` | CONFIRMADO ruta y parser; schema confirmado en parser inferior `0x84f40`. |
| Perfil usuario | `0x3c190` / `0x3bc80` | GET `/users/{id transformado}` | objeto UserInfo por `0x49450` | CONFIRMADO ruta y parser; schema confirmado en parser inferior `0x495f0`. |

Serializer de creación `0x847d0`: emite strings obligatorias `OwnerGuid`, `Region`, `GameVersion`; emite `Users` como array de strings de la colección; emite `PartyGuid`, `GameType`, `RequestedServerGuid`, `RequestedMaps` solo si sus strings no están vacías. `RequestedMaps` es una string en este serializer, no un array inferido por su nombre. El constructor `0x80e80` exige OwnerGuid, colección Users no vacía, Region y GameVersion no vacíos antes de enviar. No se atribuye un método HTTP de heartbeat ni registro/update del servidor a esos nombres: el Client64 SDK no ha demostrado esas operaciones. Los intervalos del paquete no establecen el protocolo de registro servidor.

Corrección de identidad de funciones: la evidencia inicial de calls solo establecía pares candidatos. Ghidra distingue el constructor task `0x80b90`, el ejecutor de creación `0x4eaa0`, la consulta `0x822e0` y cancelación `0x82d40`; las etiquetas de esta tabla prevalecen sobre interpretaciones preliminares.

### Evidencia auxiliar de construcción/auth

`evidence/ghidra-sdk-auxiliary.jsonl` confirma nueve funciones adicionales con el mismo hash de la DLL. `0x74010` inicializa la clave `meteoruri` a `https://v2-services-main.playhawken.com`; esto corrobora el default compilado, independiente del archivo distribuido. `0x53e60` construye telemetría como `http://collect5957hwknp.deltadna.net/collect/api/{campo consumidor +0xb0}/bulk`, agrupa eventos con umbral 2 000 000 bytes y crea task `0x53900`; no se recuperó el valor final de ese campo ni se emitió telemetría.

`0xc01b0` solicita GET `/vivox/credentials` y exige strings `Password`, `Username`, `SipRealm`, `SipUri`, `ApiUri` antes de pasar a conexión/login Vivox; dos esperas de 30 000 ms y mensajes de fallo de voz están confirmados en el código, sin probar que la voz sea requisito de juego. `0xa5be0` envía POST `Client/AddGenericID` con `{GenericId: {ServiceName: "Storm", UserId: valor}}`; `0xa5950` POST `Client/UpdateUserTitleDisplayName` con `DisplayName` string. Son formas recuperadas del request, no respuestas propuestas.

El algoritmo de URI `0xf0850` conserva bytes `A-Z a-z 0-9 - . _ ~`; otros bytes se escriben `%XX` uppercase. El login aplica dos escapes, mientras el listing individual aplica una llamada observable. La whitelist y sus bytes quedan en `auth/uri-escape-whitelist.json`; no hubo ejecución del binario para comprobar ejemplos.

### Schemas de respuesta internos recuperados

Los siguientes campos corresponden al valor `Result` dentro del envelope Meteor con `Status` y `Message`. La procedencia común es SHA-256 `f63fef02ffb517ca7844a769d03412ad82bd22198397302ee3c090c63a970c57`, ImageBase `0x180000000`. `evidence/auth/recovered-contracts.json` retiene campos, requisitos, parser RVA/offset, fuente de decompilación y xrefs de keys disponibles. Todos son CONFIRMADOS como consumo estático, NO VERIFICADOS con un servicio. «Int» describe lectura/conversión de la biblioteca JSON a 32 bits, no reglas de negocio/valores permitidos. Tipos de setter inferidos por Ghidra pueden estar fusionados con funciones idénticas de otros modelos; las keys y stores guían la interpretación.

| Modelo y función | Campos/tipos recuperados | Requisitos y límites |
|---|---|---|
| GameServerListing, `0x7a610` (file `0x79a10`) | strings: `Guid`, `MatchId`, `Region`, `GameVersion`, `ServerName`, `Map`, `GameType`, `Endpoint`, `Port`, `VoiceChannelListing`; int: `MaxUsers`, `MinUsers`, `ServerScore`, `ServerRanking`, `MatchCompletionPercent`; bool: `IsMatchmakingVisible`, `IsPublicVisible`; arrays de entradas string: `Users`, `AllowedRoles`; object: `DeveloperData` | Rechaza raíz null. Campos se leen condicionalmente por tipo, sin exigir cada uno para retornar true. AllowedRoles se transforma a string delimitada con coma en el modelo. DeveloperData enumera keys y obtiene valores mediante helper de c-string; formatos no-string no se validaron. |
| ClientSession, wrapper `0x84da0`, parser `0x84f40` (file `0x84340`) | strings: `SessionId`, `SessionKey`, `UserGuid`, `ClientAdvertisementGuid`, `AssignedServerGuid`, `AssignedServerIp`; int: `AssignedServerPort` | Exige raíz object y todos los campos con esos tipos/convertibilidad para retornar true. No establece tamaño de SessionKey, firma ni admisión en UE3. |
| ClientMatchmakingAdvertisement, wrapper `0x83fb0`, parser `0x84150` (file `0x83550`) | strings requeridas: `Guid`, `TimeCreated`, `OwnerGuid`, `Region`, `GameVersion`; numéricos: `Status`, `SkillRatingAverage`, `SkillRatingSearchMin`; array: `Users` con entradas string; strings opcionales: `GameType`, `PartyGuid`, `RequestedServerGuid`, `RequestedMaps` | Raíz object requerida. Status=1 exige strings `AssignedServerGuid`, `AssignedServerIp` y número `AssignedServerPort` convertido unsigned. Lee `SkillRatingSearchMin` dos veces y almacena en campos +8 y +0xc (xrefs `0x84300`, `0x84326`, `0x8434b`, `0x84371`); no se sustituye por una key SearchMax inventada. Significado público de estados pendiente. |
| UserInfo, wrapper `0x49450`, parser `0x495f0` (file `0x489f0`) | strings: `CreatedAt`, `DeleteToken`, `EmailAddress`, `Guid`, `LastLoggedInAt`, `LastLoggedOutAt`, `PasswordLastChangedAt`, `UpdatedAt`, `DeactivatedTime`, `GovernmentId`, `Callsign`; int: `FailedLoginAttempts`; unsigned: `Online_Time`; número: `AntiAddictionModifierNumber`; bool: `Adult`; array de strings: `Roles` | Raíz null retorna false; estos campos son condicionales por tipo y no todos se exigen. LastLoggedOutAt se lee dos veces. Presencia de campos no demuestra que producción los devolviera todos. |

**Diferencia wire/modelo local:** el SDK lee `Endpoint` (p minúscula) y `Port` string en GameServerListing, mientras el paquete UE3 refleja `EndPoint` y un puerto entero. ClientSession usa `AssignedServerIp` string y `AssignedServerPort` numérico. Los nombres del struct UE3 no se deben copiar automáticamente como keys JSON. La conversión/adaptación específica del consumidor EXE se trata en `HAWKEN_MASTER_SERVER_PROTOCOL.md`/`HAWKEN_GAME_SERVER_PROTOCOL.md`; este corte no demuestra todo su flujo.

## Cobertura adicional aportada por el padre

`evidence/remaining-network-scan.json` registra otros 74 PE y candidatos de configuración: 1 028 coincidencias léxicas, sin errores de extracción. El catálogo conserva ese ledger como evidencia complementaria por archivo/hash/offset/RVA; sus candidatos no se convierten automáticamente en endpoints operativos. APEX/PhysX, herramientas/editor, instaladores UE3, firmas/certificados y metadatos .NET explican gran parte de la lista. No se repitió el escaneo.

| Config/literal adicional | Archivo y offset | Hash | Interpretación y estado |
|---|---|---|---|
| `;IDs obtained from NVIDIA_Device_IDs_Jan_2013 from NVIDIA.com` | `Engine\Config\BaseCompat.ini` 0x68 | `83fdb7d234b9bb60937f0cb324f1d8ebbfb739be61745776da7cc31d683572c4` | CONFIRMADO texto; comentario/documentación de hardware. NO VERIFICADO uso runtime; no nuevo backend Meteor demostrado. |
| `;IDs obtained from ATI_Device_IDs from ATI.com` | `Engine\Config\BaseCompat.ini` 0x4323 | `83fdb7d234b9bb60937f0cb324f1d8ebbfb739be61745776da7cc31d683572c4` | CONFIRMADO texto; comentario/documentación de hardware. NO VERIFICADO uso runtime; no nuevo backend Meteor demostrado. |
| `; http://www.pcidatabase.com/vendor_details.php?id=0302` | `Engine\Config\BaseCompat.ini` 0x8827 | `83fdb7d234b9bb60937f0cb324f1d8ebbfb739be61745776da7cc31d683572c4` | CONFIRMADO texto; comentario/documentación de hardware. NO VERIFICADO uso runtime; no nuevo backend Meteor demostrado. |
| `EpicDefaultServerName=testtrack.epicgames.net` | `Engine\Config\BaseEditor.ini` 0x72ef | `e89b455deace0eff90cd74f09814756e5778afe64fd6729f8e670b774b19424b` | CONFIRMADO texto; editor/bug tracker stock UE3. NO VERIFICADO uso runtime; no nuevo backend Meteor demostrado. |
| `RemoteConnectionIP="10.1.20.20"` | `Engine\Config\BaseEngine.ini` 0x66c3 | `b550c921f85f41aa947e4502990e6f381163e82836ef64141aaff05cb5f222d9` | CONFIRMADO texto; stock UE3 profiling/SQL remoto. NO VERIFICADO uso runtime; no nuevo backend Meteor demostrado. |
| `RemoteConnectionIP="10.1.20.20"` | `Engine\Config\BaseEngine.ini` 0x689c | `b550c921f85f41aa947e4502990e6f381163e82836ef64141aaff05cb5f222d9` | CONFIRMADO texto; stock UE3 profiling/SQL remoto. NO VERIFICADO uso runtime; no nuevo backend Meteor demostrado. |
| `RemoteConnectionIP="10.5.5.26"` | `HawkenGame\Config\DefaultEngine.ini` 0x3843 | `6f839d73d16f512fbbf29e4d35ea12ba1e8604c8372ea72cad90dabff2526599` | CONFIRMADO texto; stock UE3 profiling/SQL remoto. NO VERIFICADO uso runtime; no nuevo backend Meteor demostrado. |
| `RemoteConnectionIP="10.5.5.26"` | `HawkenGame\Config\DefaultEngine.ini` 0x39f4 | `6f839d73d16f512fbbf29e4d35ea12ba1e8604c8372ea72cad90dabff2526599` | CONFIRMADO texto; stock UE3 profiling/SQL remoto. NO VERIFICADO uso runtime; no nuevo backend Meteor demostrado. |
| `LocalServerIP=127.0.0.1` | `HawkenGame\Config\DefaultGame.ini` 0x37a8 | `dc2a794217d13d8e7af4719c087dfbbfb4e36cbe433dd8474caaa893ddd7be00` | CONFIRMADO texto; configuración local de juego. NO VERIFICADO uso runtime; no nuevo backend Meteor demostrado. |

Orden observado de construcción de query en `0x7f0d0`: Region → GameVersion → Map → GameType → ServerName. Cada filtro no vacío se escapa una vez. La query empieza con `?` y cambia a `&` después de cada filtro añadido; sin filtros no se añade query. Estas reglas son consumo estático, no garantizan todos los filtros que soporta un servicio.

## Cotejo final de evidencia

`evidence/auth/validation.json`: PASS de once hashes originales contra inventario del padre, 337 spans exactos de candidatos y doce líneas/config offsets. `evidence/auth/integration-validation.json`: PASS de hashes Meteor/EXE, 106 spans literales del EXE con casing original, 58 spans de keys de modelos y 55 funciones Ghidra con hash correcto/decompile completo. Los documentos se comprobaron como UTF8. Estas comprobaciones validan procedencia y fidelidad estática; no prueban login, endpoints accesibles, tráfico, servidor compatible ni aceptación de una partida.


## Ampliación de cuentas, progresión y eventos

El [documento de cuentas y eventos](HAWKEN_ACCOUNT_AND_EVENT_FINDINGS.md) extiende las rutas ya catalogadas con builders/ejecutores, parsers de logros/recompensas, metadatos de paquetes y separación de callbacks/gameplay/telemetría. Incluye rutas PUT de stats con seguimiento TransactionId, GET de asignaciones diarias, POST de logros cliente y las APIs PlayFab de datos/eventos, sin afirmar uso runtime. El diseño solicitado exige Steam legítimo y permite créditos internos ganados jugando, con reglas fijadas por el servidor, sin moneda premium, compras con dinero ni monetización. Las rutas históricas de store/balances quedan como evidencia del original y deuda de compatibilidad.

## Consumidores UPK recuperados y límites de la selección de servicio

Los seis paquetes exportados con UELib 1.13.0 se leen como derivados estáticos: `evidence/upk-normalized-copies.json` vincula original, descomprimido y normalizado, y acredita conservación del payload y posiciones virtuales. No se llama RVA a una posición UPK ni se convierte un offset virtual en offset bruto del original comprimido. `evidence/auth/upk-auth-consumers.json` conserva 26 registros seleccionados y hashes de sus `.uc`; los índices de export se resuelven por path original, porque `object_ordinal` y `NetIndex` del manifest **no equivalen en general al índice de export**. `evidence/game/upk-native-token-map.json` permite interpretar concatenaciones/predicados `__NFUN_n__` desde declaraciones Core reales, sin fingir recuperación de los cuerpos nativos.

Originales: OnlineSubsystemMeteor SHA-256 `bab9ad82bd5fae5afe30707513caa27605c7ece8f296461678ae07cd759ed310`; HawkenGame `d1a33a064de4451e5a39e13a28e4a8f2913ce6d9b54ca65bae47118d32849ed0`. Engine stock `8fff59b404ea4f9dd41330da618432fe50e4a26ce7487379efeb76ce0c4e859c`; IpDrv stock `432087362da101de1189e88a260e4a18c016320b8b9ccd9911df59eebe04b4ef`. Cada registro retiene archivo original, hash, exporttable/serial virtual, firma/flags y archivo `.uc`/hash.

| Consumidor | Localización y decompilado | Pregunta cerrada / límite |
|---|---|---|
| `OnlineSubsystemMeteor.Init` | `#265`; virtual `0x59c95`; [script](<F:/HawkenServer/research/2026-10-10/evidence/upk/packages/OnlineSubsystemMeteor/000386-OnlineSubsystemMeteor.OnlineSubsystemMeteor.Init.uc>) | CONFIRMADO declaración nativa `native event bool Init`, flags `0x20c00`; no cuerpo script. Binding EXE `3ff0f0` → slot `0x288` → `bc3510`, CONFIRMADO por UClass/constructor/vtable; lector SDK/override EXE ya confirmado por evidencia INI. |
| `OnlineSubsystemMeteor.PostInit` | `#268`; virtual `0x59d26`; [script](<F:/HawkenServer/research/2026-10-10/evidence/upk/packages/OnlineSubsystemMeteor/000388-OnlineSubsystemMeteor.OnlineSubsystemMeteor.PostInit.uc>) | CONFIRMADO cuerpo script: llama inicializadores nativos de listing → voz → item cache → PVP globals; combina éxito local. No asigna URI. |
| `OnlineSubsystemMeteor.GetSteamAuthSessionTicket` | `#1621`; virtual `0x6e903`; [script](<F:/HawkenServer/research/2026-10-10/evidence/upk/packages/OnlineSubsystemMeteor/001742-OnlineSubsystemMeteor.OnlineSubsystemMeteor.GetSteamAuthSessionTicket.uc>) | CONFIRMADO firma nativa retorna string; flags `0x20401`, NativeToken 0. Binding EXE `b73120` llama directamente al productor `bc2130`; CONFIRMADO enlace con ticket Steam/Base64url sin padding, ticket real NO VERIFICADO. |
| `OnlineRecentPlayersListMeteor.ReadPersistantData` | `#2814`; virtual `0x81688`; [script](<F:/HawkenServer/research/2026-10-10/evidence/upk/packages/OnlineSubsystemMeteor/002934-OnlineSubsystemMeteor.OnlineRecentPlayersListMeteor.ReadPersistantData.uc>) | CONFIRMADO añade `Player.RecentPlayers`, registra callback y llama ReadUserSettings categoría 0. |
| `OnlineRecentPlayersListMeteor.WritePersistentData` | `#2827`; virtual `0x81a74`; [script](<F:/HawkenServer/research/2026-10-10/evidence/upk/packages/OnlineSubsystemMeteor/002944-OnlineSubsystemMeteor.OnlineRecentPlayersListMeteor.WritePersistentData.uc>) | CONFIRMADO serializa lista de ID/nickname y llama SetSettingString(`Player.RecentPlayers`) → WriteUserSettings categoría 0; no prueba persistencia universal de progreso. |
| `R_OnlineConfig.GetBaseWebURL` | `#37701`; virtual `0xbcc542`; [script](<F:/HawkenServer/research/2026-10-10/evidence/upk/packages/HawkenGame/041434-HawkenGame.R_OnlineConfig.GetBaseWebURL.uc>) | CONFIRMADO retorna R_OnlineConfig.default.BaseWebURL; propiedad `config(OnlineConfig)` y default web de playhawken. No cambia MeteorUri. |
| `R_GFx_MechMenu_Base.ConcatStormLoginToURL` | `#22954`; virtual `0x947dea`; [script](<F:/HawkenServer/research/2026-10-10/evidence/upk/packages/HawkenGame/026684-HawkenGame.R_GFx_MechMenu_Base.ConcatStormLoginToURL.uc>) | CONFIRMADO agrega storm_id; SteamID distinto ZeroId agrega steam_id y ticket string si no vacío; consumidor web, no nueva base API ni esquema de admisión de juego. |
| `R_GFx_MainMenu.PlayerLogin` | `#26223`; virtual `0x9b7074`; [script](<F:/HawkenServer/research/2026-10-10/evidence/upk/packages/HawkenGame/029954-HawkenGame.R_GFx_MainMenu.PlayerLogin.uc>) | CONFIRMADO script entrega PlayerName/Password a OnlineSub.PlayerInterface.Login(0,...). No constituye verificación de autenticación en servidor. |

La búsqueda textual acotada de `MeteorUri`, `StormUri`, `ServiceUri`, `StormConsumerSettings` y `MasterServerAddress` en declaraciones/cuerpos exportados de los seis paquetes no encontró esos términos. Se examinaron records filtrados por prefijo de paquete: 352 Core, 6933 Engine, 833 IpDrv, 992 OnlineSubsystemMeteor y 10855 HawkenGame; Startup tiene 0 records con ese prefijo de clase/función en este conjunto. El filtro incluye imports del mismo namespace y no equivale al recuento de exports positivos. El audit independiente [decompilation-scope-validation.json](<F:/HawkenServer/research/2026-10-10/evidence/upk/decompilation-scope-validation.json>) distingue 334 exports positivos + 18 imports de Core, y 6924 exports positivos + 80 imports de Engine; 9 de estos últimos imports tienen prefijo Engine y explican los 6933 records examinados. Este resultado **no demuestra ausencia de un mecanismo nativo alternativo**. Las firmas nativas Init/Steam/Storm_Login se siguen hasta la tabla de bindings del EXE en `evidence/auth/upk-native-bindings.json`; NativeToken 0 no significa que su cuerpo haya sido descompilado. No se recuperó una opción URI de script que resuelva la contradicción INI-only del camino EXE observado.

`evidence/auth/upk-endpoint-ledger.json` añade destinos/literales y construcciones de scripts. Son hallazgos del original, sin afirmar hosts activos ni endpoints propios del futuro sistema:

| Destino o construcción | Fuente y posición virtual de literal | Interpretación / estado |
|---|---|---|
| `http://www.epicgames.com` | `CheatManager.TestHttp`, export `#10058`, virtual `0x3b23ca`; [script](<F:/HawkenServer/research/2026-10-10/evidence/upk/packages/Engine/010251-Engine.CheatManager.TestHttp.uc>) | Stock UE3 diagnóstico/servidor web; CONFIRMADO construcción, ejecución NO VERIFICADA. |
| `http://e277e173.ngrok.io` | `R_ObjectFormatter.GetURL`, export `#9177`, virtual `0x7c9167`; [script](<F:/HawkenServer/research/2026-10-10/evidence/upk/packages/HawkenGame/012909-HawkenGame.R_ObjectFormatter.GetURL.uc>) | CONFIRMADO literal/consumer de script; uso runtime NO VERIFICADO. |
| `http://collect5957hwknp.deltadna.net/collect/api/22554436496886934584544905414444/bulk` | `R_DeltaDNAObjectFormatter.GetURL`, export `#18316`, virtual `0x8b8388`; [script](<F:/HawkenServer/research/2026-10-10/evidence/upk/packages/HawkenGame/022048-HawkenGame.R_DeltaDNAObjectFormatter.GetURL.uc>) | CONFIRMADO literal/consumer de script; uso runtime NO VERIFICADO. |
| `http://collect5957hwknp.deltadna.net/collect/api/22554436496886934584544905414444` | `R_DeltaDNAObjectFormatter.GetURL`, export `#18316`, virtual `0x8b83e4`; [script](<F:/HawkenServer/research/2026-10-10/evidence/upk/packages/HawkenGame/022048-HawkenGame.R_DeltaDNAObjectFormatter.GetURL.uc>) | CONFIRMADO literal/consumer de script; uso runtime NO VERIFICADO. |
| `https://playhawken.zendesk.com/entries/22834512-information-needed-in-your-bug-report` | `R_GFx_Options.ReportBug`, export `#19858`, virtual `0x8e71ac`; [script](<F:/HawkenServer/research/2026-10-10/evidence/upk/packages/HawkenGame/023592-HawkenGame.R_GFx_Options.ReportBug.uc>) | CONFIRMADO literal/consumer de script; uso runtime NO VERIFICADO. |
| `/currency` | `R_GFx_MechMenu_Base.BuyMeteorCredits`, export `#22973`, virtual `0x948662`; [script](<F:/HawkenServer/research/2026-10-10/evidence/upk/packages/HawkenGame/026706-HawkenGame.R_GFx_MechMenu_Base.BuyMeteorCredits.uc>) | CONFIRMADO literal/consumer de script; uso runtime NO VERIFICADO. |
| `/redeem-code` | `R_GFx_MainMenu.OpenRedeemCodeURL`, export `#26761`, virtual `0x9c69b8`; [script](<F:/HawkenServer/research/2026-10-10/evidence/upk/packages/HawkenGame/030494-HawkenGame.R_GFx_MainMenu.OpenRedeemCodeURL.uc>) | CONFIRMADO literal/consumer de script; uso runtime NO VERIFICADO. |
| `/#enlist/` | `R_LoginDialog.HandleLoginDialogCreateAccount`, export `#34907`, virtual `0xaa90eb`; [script](<F:/HawkenServer/research/2026-10-10/evidence/upk/packages/HawkenGame/038641-HawkenGame.R_LoginDialog.HandleLoginDialogCreateAccount.uc>) | CONFIRMADO literal/consumer de script; uso runtime NO VERIFICADO. |
| `/password-reset` | `R_LoginDialog.HandleOpenPasswordRecovery`, export `#34911`, virtual `0xaa92aa`; [script](<F:/HawkenServer/research/2026-10-10/evidence/upk/packages/HawkenGame/038645-HawkenGame.R_LoginDialog.HandleOpenPasswordRecovery.uc>) | CONFIRMADO literal/consumer de script; uso runtime NO VERIFICADO. |
| `http://{ServerName o IP local sin puerto}[:ListenPort cuando distinto de 80]` | `WebServer.PostBeginPlay`, export `#4164`, virtual `0xa96a3`; [script](<F:/HawkenServer/research/2026-10-10/evidence/upk/packages/IpDrv/004372-IpDrv.WebServer.PostBeginPlay.uc>) | Stock UE3 diagnóstico/servidor web; CONFIRMADO construcción, ejecución NO VERIFICADA. |

El `GetURL(Bulk)` de R_DeltaDNAObjectFormatter retorna la ruta completa con el identificador `22554436496886934584544905414444`; ese valor está confirmado **en el script**, sin atribuirlo al campo dinámico `+0xb0` del consumidor C++ SDK. El formatter genérico R_ObjectFormatter retorna el literal ngrok: selección/uso real quedan NO VERIFICADOS. BaseWebURL sirve a recuperación de contraseña, creación de cuenta web, códigos y compra histórica, y no controla por esa evidencia la base Meteor. `/currency` y `BuyMeteorCredits` documentan comportamiento histórico y siguen excluidos del diseño futuro: se permiten créditos internos ganados jugando, con reglas fijadas por el servidor, sin moneda premium, compras con dinero ni monetización. Las rutas históricas de PayPal tampoco forman parte del diseño.

### Cierre del dispatcher nativo y del escritor global

`evidence/auth/ini-endpoint-native-closure.json` y `upk-native-bindings.json` cierran **CONFIRMADO estático** el salto de Init: registro `b8b230` (offset `b8a630`) pasa literal UTF16 `OnlineSubsystemMeteor` (`168a9e2`/`1689de2`) y callback constructor `b8f170` (`b8e570`); éste instala vtable `169c610`, cuyo slot `0x288` (`169c898`/`169bc98`) contiene `bc3510`. El wrapper Init `3ff0f0` (`3fe4f0`) despacha exactamente ese slot. No se trató el teardown `b7ce00` como constructor ni se atribuyó un cuerpo script a la declaración nativa. Las cuatro decompilaciones de `ghidra-main-upk-auth-helpers.jsonl` y las del lote `ghidra-main-upk-native-consumers.jsonl` conservan el SHA original EXE `4a861ecb4ac7add9554f8daf8f98ad2c12b9cb6c1865a83e8be808beb46ea8a2`.

Las ocho referencias conservadas de las globals `1d39010`/`1d39018` apuntan al selector, al buffer de arranque y al teardown `128cd10` (offset `128c110`). Este último vacía/freea el FString, sin leer configuración. `c6f890` obtiene un buffer mediante `b3bd0(param1)` y lo pasa a `c70e20`; `af610`/`af820` copian el texto encontrado para el literal `MeteorURI=`. No apareció otro lector efectivo de API URI en esos consumidores; el resultado se limita a referencias conservadas y no excluye escrituras calculadas/indirectas. BaseWebURL conserva su frontera web/UI. El origen queda cerrado desde `GetCommandLineW` en `c77450` hasta el parser, con registro RCX preservado en las dos ramas de `c6fa40` y salida de `b3bd0` pasada como RDX a `c70e20`; se cotejaron instrucciones contra el original. El argumento es un hecho histórico excluido del diseño solicitado. La contradicción INI-only del camino Init sigue vigente.

## Barrido adicional de paquetes y clasificación de candidatos

La agregación `evidence/upk/additional-code-package-scope.json`, basada en el barrido existente de 360 UPK, identifica fuera de los seis paquetes anteriores 2581 exports propios Class/Function (557 Class, 2024 Function) en 70 paquetes. Son índices/payloads localizados, no 2024 cuerpos ya recuperados. La búsqueda textual anterior queda limitada a los seis derivados, y no se extiende por inferencia a este nuevo conjunto. No se volvió a escanear, descomprimir ni acceder a la red para esta clasificación.

`evidence/auth/additional-upk-literal-classification.json` clasifica las 12 URLs, 19 dominios léxicos y dos candidatos IPv4 únicos externos. Cada entrada conserva un representante con archivo original, SHA-256, export/owner, inicio del texto portador y offset **virtual del primer literal exacto** calculado con el ancho de su encoding, offset/hash del bloque zlib original y contexto; todas sus ocurrencias siguen enlazadas a la agregación original (3431 coincidencias, incluidas repeticiones en assets). La presencia del literal/owner es **CONFIRMADA** por esa evidencia; su actividad en red queda **NO VERIFICADA**. La clasificación de consumidores stock pendientes es **PROBABLE**, hasta disponer del cuerpo de los paquetes adicionales. No se atribuye ninguna de estas direcciones a Meteor.

| URL exacta del barrido | Owner representativo / virtual | Clasificación y límite |
|---|---|---|
| `http://api.twitter.com/1/statuses/update.json` | `UDKBase.CloudPC.OnTwitterAuthorizeComplete` #413 / `0x52fef` | Stock UDKBase; TRM_Post y guard GetNumAccounts()>0 recuperados, transporte nativo no verificado. |
| `http://ns.adobe.com/xap/1.0/` | `RobotsMainMenu.UI.UI_Visor_Loader` #5520 / `0x1ecaccf` | Metadata XMP/RDF/Dublin Core de SwfMovie; no solicitud acreditada. |
| `http://ns.adobe.com/xap/1.0/mm/` | `RobotsMainMenu.UI.UI_Visor_Loader` #5520 / `0x1ecaeb4` | Metadata XMP/RDF/Dublin Core de SwfMovie; no solicitud acreditada. |
| `http://ns.adobe.com/xap/1.0/sType/ResourceRef#` | `RobotsMainMenu.UI.UI_Visor_Loader` #5520 / `0x1ecaee2` | Metadata XMP/RDF/Dublin Core de SwfMovie; no solicitud acreditada. |
| `http://papersound.bandcamp.com/album/hawken` | `RobotsMainMenu.UI_Menus.Panels.Garage_Emotes` #5524 / `0x2054da4` | Enlace al álbum dentro de SwfMovie; ejecución del enlace no probada. |
| `http://purl.org/dc/elements/1.1/` | `RobotsMainMenu.UI.UI_Visor_Loader` #5520 / `0x1ecae1e` | Metadata XMP/RDF/Dublin Core de SwfMovie; no solicitud acreditada. |
| `http://test.com` | `RobotsMainMenu.UI_Menus.Panels.Deploy` #5523 / `0x1f79ae0` | Literal de prueba de asset UI; falta consumidor ActionScript. |
| `http://www.epicgames.com` | `GameFramework.GameCheatManager.TestHttp` #851 / `0x6eda0` | Stock GameFramework TestHttp; GET condicional confirmado en el anexo de cuerpos. |
| `http://www.tweenlite.com.` | `RobotsMainMenu.UI.UI_Visor_Loader` #5520 / `0x1ee6f1a` | Aviso de actualización de biblioteca UI; se conserva el punto final del texto. |
| `http://www.TweenMax.com.` | `RobotsMainMenu.UI.UI_Visor_Loader` #5520 / `0x1ee7bfd` | Aviso de actualización de biblioteca UI; se conserva el punto final del texto. |
| `http://www.w3.org/1999/02/22-rdf-syntax-ns#` | `RobotsMainMenu.UI.UI_Visor_Loader` #5520 / `0x1ecac78` | Metadata XMP/RDF/Dublin Core de SwfMovie; no solicitud acreditada. |
| `http://yourkidsartsucks.com/wp-content/uploads/2011/10/Funny_Art_223.jpg` | `UDKBase.CloudPC.CloudGameFacebook` #395 / `0x526fa` | Stock UDKBase; URL JPG como parámetro picture del diálogo feed, no API de cuenta. |

| Dominio léxico exacto | Clasificación / observación |
|---|---|
| `AGh3.GG` | Coincidencia léxica dentro de ShaderCache/Level; no hay consumidor de red demostrado. |
| `api.twitter.com` | Payload UDKBase.CloudPC.OnTwitterAuthorizeComplete; falta cuerpo para cerrar verbo/guard/selección. |
| `DXBC.Gg` | Coincidencia léxica dentro de ShaderCache/Level; no hay consumidor de red demostrado. |
| `Fh3.Gg` | Coincidencia léxica dentro de ShaderCache/Level; no hay consumidor de red demostrado. |
| `JFh3.Gg` | Coincidencia léxica dentro de ShaderCache/Level; no hay consumidor de red demostrado. |
| `LocalConst7.gg` | Coincidencia léxica dentro de ShaderCache/Level; no hay consumidor de red demostrado. |
| `LocalConst8.gg` | Coincidencia léxica dentro de ShaderCache/Level; no hay consumidor de red demostrado. |
| `LOCPlayhawken.com` | Prefijo LOC en texto de UI, no hostname limpio seleccionado por Meteor. |
| `ns.adobe.com` | Namespace XMP/RDF/Dublin Core en SwfMovie; no demuestra solicitud de red. |
| `papersound.bandcamp.com` | Referencia al álbum de Hawken dentro de SwfMovie; ejecución/enlace activo no probado. |
| `purl.org` | Namespace XMP/RDF/Dublin Core en SwfMovie; no demuestra solicitud de red. |
| `Temporary2.gg` | Coincidencia léxica dentro de ShaderCache/Level; no hay consumidor de red demostrado. |
| `Temporary5.gg` | Coincidencia léxica dentro de ShaderCache/Level; no hay consumidor de red demostrado. |
| `test.com` | Literal de prueba en SwfMovie; falta análisis de ActionScript para acreditar consumidor. |
| `www.epicgames.com` | Payload de GameFramework.GameCheatManager.TestHttp; cuerpo extra aún pendiente, sin atribución a Meteor. |
| `www.google.com` | Payload UDKBase.CloudPC.CloudGameFacebook; dominio/imagen de demostración, no base Meteor. |
| `www.w3.org` | Namespace XMP/RDF/Dublin Core en SwfMovie; no demuestra solicitud de red. |
| `Xiph.Org` | Texto de proveedor/códec en SoundNodeWave; no acredita endpoint operativo. |
| `yourkidsartsucks.com` | Payload UDKBase.CloudPC.CloudGameFacebook; dominio/imagen de demostración, no base Meteor. |

Los valores de shader `LocalConst7.gg`, `LocalConst8.gg`, `Temporary2.gg` y `Temporary5.gg` aparecen como selección de componentes `.gg` en expresiones; no se consideran dominios de servicio. `AGh3.GG`, `Fh3.Gg`, `JFh3.Gg` y `DXBC.Gg` son coincidencias léxicas en Level/ShaderCache sin consumidor de red demostrado. `Xiph.Org` aparece en el texto `Xiph.Org libVorbis I 20101101 (Schaufenugget)`, metadata del códec de audio. `LOCPlayhawken.com` aparece en `LOCPlayhawken.com to give us money!!!`, texto de localización UI, y no se normaliza a un nuevo dominio. `3.8.8.0` está en el texto de loader UI `Firmware v3.8.8.0 (20455)`; `3.4.5.7` está en un StaticMesh. Ninguno prueba dirección IP de backend ni puerto. Los nombres/códigos de versión se retienen como hallazgos, sin convertirlos en endpoints activos.

La revisión selectiva de los nuevos derivados priorizó `UDKBase.CloudPC` (CloudGameFacebook/OnTwitterAuthorizeComplete/OnFBAuthComplete), `GameFramework.GameCheatManager.TestHttp` y opciones de GFxUI. La mera presencia de esos exports no acredita integración social de Hawken, autoridad de cuenta ni una opción alternativa de base Meteor. Los cuerpos nativos y los assets SWF siguen siendo fronteras distintas del cuerpo UnrealScript.

### Enlaces nativos de listing y eventos SDK

`evidence/auth/core-listing-event-getters.json` conserva bytes/offsets de slots y RTTI de la DLL SHA-256 `f63fef02ffb517ca7844a769d03412ad82bd22198397302ee3c090c63a970c57`. Core vtable `0x1a2b88` slot `+0x60` → getter `0x1f990` (offset `0x1ed90`) devuelve la interfaz `core+0xd8+0x20`; su vtable `0x1a1d18` tiene RTTI `GameManagerConsumer`, slot `+8` → thunk `0xbd70` (offset `0xb170`) ajusta this−0x20 → `0xb7a0` (offset `0xaba0`) → `0x7f0d0`, cuyo GET `/gameServerListings` y filtros ya se recuperaron. **CONFIRMADO** enlace estático con el contrato de listing, sin nueva base de servicio ni prueba runtime.

Core slot `+0x50` → getter `0x1f950` (offset `0x1ed50`) devuelve interfaz `core+0xc8+0xd8`; constructor `0x4960` instala vtable secundaria `0x1a1a00`, RTTI `EventConsumer`; slot `+0x18` → thunk `0x976c` (offset `0x8b6c`) ajusta this−0xd8 → `0x4d70` (offset `0x4170`). Este último prepara un EventObject y llama `0x51740`; el nombre del ejecutable puede derivarse por GetModuleFileNameA. Las dos decompilaciones completas de `evidence/ghidra-sdk-upk-linked-consumers.jsonl` llevan el SHA original. **CONFIRMADO** enlace estático: `0x51740` valida/fecha/encola y retorna aceptación local, sin HTTP inmediato. El worker y el POST diferido se recuperaron en el apartado siguiente; la confirmación local no es respuesta remota.

### Cuerpos adicionales de consumidores stock recuperados

`evidence/auth/additional-upk-consumers.json` enlaza nueve cuerpos/declaraciones seleccionados con el índice de export original del scan360, hash original, offset virtual, flags/ScriptSize y hash de cada `.uc` cotejado con su manifest. UDKBase SHA-256 `4e2e6caf4141f8f01d6d1f45c9bcb570991b63c37b3d89eb1e0ba79f55d14064`; GameFramework `d51275d3d52e4461feb98b34e3bc64264130efb0618f1fe6b7981730ad63ab8b`; enum/firma Engine `8fff59b404ea4f9dd41330da618432fe50e4a26ce7487379efeb76ce0c4e859c`. Son ejemplos/superficies stock: **CONFIRMADO** contenido script y llamadas, uso por el juego Hawken y actividad remota **NO VERIFICADO**.

| Consumer / export y virtual | Observación recuperada | Frontera |
|---|---|---|
| `UDKBase.CloudPC.PostBeginPlay` #345 / `0x50e1d` | Obtiene interfaces CloudStorage/Facebook/Twitter, registra delegates; Twitter.AuthorizeAccounts y CloudGetDocs. | Inicialización del ejemplo CloudPC; no asociación Steam/Meteor acreditada. |
| `CloudPC.OnTwitterAuthorizeComplete` #413 / `0x52ebe` | Si GetNumAccounts()>0, forma pares `status`, `UE3 auto tweet. Ignore me.` y llama TwitterRequest(`http://api.twitter.com/1/statuses/update.json`, pares, 1, 0). | Enum real Engine.ETwitterRequestMethod confirma 1=TRM_Post y AccountIndex=0. Firma native no recupera implementación ni prueba respuesta/credenciales. |
| `CloudPC.CloudGameTwitter` #404 / `0x52be1` | Si CanShowTweetUI, ShowTweetUI(`Default tweet!`, `http://www.epicgames.com`, `Icon`). | Enlace web dentro de interfaz stock; no petición API Meteor. |
| `CloudPC.CloudGameFacebook` #395 / `0x52586` | Ante falta de autorización llama Authorize y retorna; en rama autorizada lista amigos y llama FacebookDialog(`feed`, pares). Pares incluyen link=`www.google.com`, picture=URL JPG del catálogo, message/name/caption/description. | Contenido de demostración para feed; Google/JPG son campos de contenido, no servidores de cuenta Hawken. |
| `CloudPC.OnFBAuthComplete` #381 / `0x520e9`; `OnTwitterRequestComplete` #408 / `0x52d84` | Primero limpia bIsFBAuthenticating; segundo muestra Result.bSuccessful y Data.StringValue en log del ejemplo. | Callbacks locales; ningún resultado acredita login Steam/Meteor ni autoridad de progreso. No se recopilaron resultados reales. |
| `GameFramework.GameCheatManager.TestHttp` #851 / `0x6e9a3` | Request con URL/Verb/Payload recibidos; con bSendParallelRequest crea otra petición `http://www.epicgames.com`, verbo `GET`, header `Test:Value`, callback y ProcessRequest. | Diagnóstico stock; CONFIRMADO llamada GET condicional, ejecución real NO VERIFICADO. |
| `Engine.TwitterIntegrationBase` #30714 / `0x57b777`; TwitterRequest #30713 / `0x57b722` | Enum TRM_Get=0, TRM_Post=1, TRM_Delete=2; native bool TwitterRequest(URL, ParamKeysAndValues, RequestMethod, AccountIndex). | Declaración/enum; no cuerpo del transporte nativo ni proveedor configurado demostrado. |

La revisión textual de 203 records propios GFxUI para MeteorUri/StormConsumerSettings/BaseWebURL/AuthTicket/Login/Steam/Http/URL/TwitterRequest/FacebookDialog sólo produjo el substring `url` en la propiedad `bBlurLesserMovies` de GFxMoviePlayer. No es un consumidor URI. Se conservan el hallazgo y los términos en el ledger, sin convertir ese negativo en ausencia de funciones nativas o de ActionScript dentro de SwfMovie. No apareció un setter Meteor ni lectura alternativa del INI en estos cuerpos adicionales seleccionados.

### Transporte diferido de EventConsumer Meteor

`evidence/auth/event-queue-transport.json` cierra **CONFIRMADO estático** el canal: DLL original SHA-256 `f63fef02ffb517ca7844a769d03412ad82bd22198397302ee3c090c63a970c57`, tres decompilaciones completas en `evidence/ghidra-sdk-upk-event-worker.jsonl` más 427 spans de instrucciones adicionales cotejados contra el original para los helpers de envío/task/array. `0x51740` (offset `0x50b40`) exige strings no vacías Producer.Id/Type, Subject.Id/Type y Verb; añade `TimeCreated` y fechas, encola bajo mutex y retorna valores locales 200/200/202 con el mensaje de aceptación diferida. Esos valores no provienen de un servidor.

Inicio `0x515f0` (offset `0x509f0`) configura intervalo 5000 y lanza callback `0x51fb0` → worker `0x51fd0` (offset `0x513d0`). Éste usa GetTickCount y el intervalo para llamar a flush `0x526a0` (offset `0x51aa0`), esperando con helper/valor 100 mientras está por debajo del intervalo; también vacía al salir. La comparación parte de tick previo 0, por lo que no se afirma un retraso inicial obligatorio de cinco segundos.

Flush copia y vacía la cola bajo mutex antes de enviar. Compone URI con core `0x746c0` y consumer+0x70 (ruta `/gameClientEvent` asignada por constructor `0x4960`). Llama al helper directo `0x52560` (offset `0x51960`) → WebClient POST `0x6e900`; la rama de executor crea task `0x52170` (offset `0x51570`), cuya vtable `0x1a49b0` slot +8 → `0x522d0` (offset `0x516d0`) llama al mismo helper POST. Se verifica el puntero del slot y los bytes de ambos saltos; no se deduce el verbo por el nombre de la ruta.

| Contrato wire de este canal | Observación / límite |
|---|---|
| POST `{base-Meteor}/gameClientEvent` | JSON raíz **array** de objetos serializado con Json::FastWriter; helper de índice `0x59d90` acepta null/tag6 y construye array/tag6. Base comparte la selección Meteor ya trazada; no constituye un MasterServer de directorio nuevo. |
| `Producer:{Id:string,Type:string}` y `Subject:{Id:string,Type:string}` | Ambos pares obligatorios no vacíos en validación local; Id/Type conservan mayúsculas. Literales RVA/offset `0x1a28f0`/`0x1a18f0` y `0x1a3914`/`0x1a2914`. |
| `Verb:string` | Campo obligatorio, RVA/offset `0x1a2508`/`0x1a1508`. El flush omite del array Meteor cualquier Verb donde strstr encuentre `DeltaDNA_`; es búsqueda de substring, no sólo prefijo. |
| `Target:{Id:string,Type:string}` | Se incluye únicamente cuando ambos strings de target no están vacíos. |
| `Timestamp:string` | Formato UTC creado con _gmtime64_s y `%04d-%02d-%02dT%02d:%02d:%02d.%03u`; ese formato no añade sufijo Z. Es fecha local generada, no reloj de servidor. |
| `Data:object` opcional | EventData.GetDataString debe ser no vacío, parsear correctamente y tener tag7/object. Array/string/JSON inválido no entra como Data en este camino. Literal RVA/offset `0x1a256c`/`0x1a156c`; incluye TimeCreated añadido localmente. |
| Batch | Acumula tamaño de cada objeto serializado+10; con acumulado<16000 añade al array. Al alcanzar/superar16000 serializa el array previo y abre otro con el evento actual. No se acredita un límite estricto de16000bytes para un evento individual grande. |
| Resultado y entrega | Se propaga ApiResult del último batch; no se recuperó un acknowledgement por evento. La cola ya se vació antes de enviar: no se observó reinserción, backoff ni persistencia durable en este tramo. Recepción y pérdida/reintento efectivo quedan NO VERIFICADOS. |

Después del envío Meteor, si consumer+0xd0 es cero, el flush llama al sender DeltaDNA `0x53e60` con la lista drenada. Ese sender y su `/bulk`, eventList y batching2MB ya estaban recuperados; no se igualan a este array Meteor de16000 ni al wrapper PlayFab WritePlayerEvent. Los callbacks gameplay, la aceptación local de cola y las respuestas de cada servicio siguen siendo cosas distintas. Ninguno de estos canales prueba autoridad de XP, desbloqueos o sanciones, ni una salida administrativa webhook/SSE/WebSocket.
