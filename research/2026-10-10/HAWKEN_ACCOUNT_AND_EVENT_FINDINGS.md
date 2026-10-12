# HAWKEN: cuentas, progresión y eventos — hallazgos estáticos

Fecha: 2026-10-10. Esta investigación alimenta el plan del proyecto; no implementa servicios ni ejecuta el cliente. Reutiliza el inventario, los paquetes descomprimidos y las sesiones de análisis del coordinador. Ninguna conexión, DNS, credencial o ticket real fue obtenido.

## Alcance, identidad y estados

**CONFIRMADO** significa bytes, metadatos o instrucciones observables en estos artefactos; no demuestra que un servidor histórico ejecutase una operación. **PROBABLE** identifica una interpretación apoyada por nombres, tipos y conexiones parciales. **NO VERIFICADO** señala contratos, autoridad, persistencia, permisos, resultados y comportamiento real todavía no demostrados. Los nombres erróneos de clases/setters introducidos por la deduplicación de funciones en Ghidra no sustituyen la evidencia de claves y offsets.

| Referencia | Archivo original | SHA-256 original | Dirección usada |
|---|---|---|---|
| SDK | `Binaries/Win64/Meteor.Sdk.Cpp.Win32.Client64.dll` | `f63fef02ffb517ca7844a769d03412ad82bd22198397302ee3c090c63a970c57` | ImageBase `0x180000000`; funciones seleccionadas `.text`: offset original = RVA − `0xc00` |
| EXE | `Binaries/Win64/HawkenGame-Win64-Shipping.exe` | `4a861ecb4ac7add9554f8daf8f98ad2c12b9cb6c1865a83e8be808beb46ea8a2` | ImageBase `0x140000000`; funciones citadas: offset original = RVA − `0xc00` |
| OSS | `HawkenGame/CookedPCConsole/OnlineSubsystemMeteor.upk` | `bab9ad82bd5fae5afe30707513caa27605c7ece8f296461678ae07cd759ed310` | Offset virtual del paquete descomprimido; SHA derivado `0f2d3ea36450c0b8b850c8cb5723304b819f2e328f6a65ed045912415534f151` |
| GAME | `HawkenGame/CookedPCConsole/HawkenGame.upk` | `d1a33a064de4451e5a39e13a28e4a8f2913ce6d9b54ca65bae47118d32849ed0` | Offset virtual del paquete descomprimido; SHA derivado `d2922c505a5a6133b20e82b3acf51200479f5f64aa9ca59ed82822bbaee3cc11` |

Cada referencia de las tablas siguientes hereda archivo/hash de esta tabla. Los offsets UPK **no** son offsets del archivo comprimido distribuido. Los hijos de una UFunction incluyen parámetros y locales; sin flags de propiedad/bytecode no se identifica automáticamente cada hijo como parámetro de entrada. Evidencia principal: [modelos recuperados](evidence/auth/account-models.json), [ledger](evidence/auth/account-event-ledger.json), [funciones SDK](evidence/ghidra-sdk-account-events.jsonl), [paquete OSS enfocado](evidence/auth/account-package-OnlineSubsystemMeteor.upk.json), [paquete GAME enfocado](evidence/auth/account-package-HawkenGame.upk.json).

## Requisitos del usuario, separados de la arqueología

El diseño solicitado exige Steam legítimo y autenticación verificada, cuentas/entradas en SQL, servicios en Linux, seguimiento de cuentas, baneos administrativos, desbloqueos ganados jugando, eventos exponibles y comandos/mensajes personalizados. El MasterServer sólo lista partidas por región; el Server Creator es un producto descargable separado con dependencias y dashboard de mapas/modos/eventos/XP/bots. **Revisión del usuario:** XP/créditos/cosméticos ganados y rates/eventos globales bajo el servicio central de cuentas/progreso del operador Master; Creator no concede progreso global. Cosméticos antes vendidos son candidatos sujetos a catálogo/grant/equipamiento verificados; sin compras con dinero real ni monetización. URI por INI y Steam normal, sin launcher; DLL puente adicional como propuesta aún no probada, sin editar binarios originales en disco. UPK nuevos/cook aplazados; integridad SHA de originales. Son requisitos del proyecto, no conclusiones de estos binarios. Las rutas premium/pagos históricas no autorizan su implementación ni una excepción a Steam.

No se ha demostrado una integración Steam operativa, una vinculación de cuentas aceptada por Valve, acceso a APIs protegidas o capacidad legítima para validar los tickets antiguos de este título. [Fuentes primarias investigadas por el coordinador](evidence/steam-integration-primary-sources.json) y [protocolo de autenticación](HAWKEN_AUTH_PROTOCOL.md) mantienen esa frontera.

## Identidad Steam y conexión con la cuenta

| Evidencia / ubicación | Observación | Interpretación y estado |
|---|---|---|
| `Binaries/Win64/steam_appid.txt`, offset `0x0`, 6 bytes ASCII; SHA `7f8af64056cc012f81841069a6bb93ad471e5f738a82e05a4981030a6da34233` | Contenido exacto `271290`, sin salto final. | **CONFIRMADO** AppID declarado por este archivo; no prueba lanzamiento/autorización ni permisos backend. |
| EXE import normal `steam_api64.dll`: `SteamAPI_Init`, IAT RVA `0x128ff40`, nombre RVA `0x1ad9a36`; `SteamAPI_RestartAppIfNecessary`, IAT `0x128ff30`. | La tabla PE importa inicialización y relanzamiento Steam. | **CONFIRMADO** enlace estático; resultado de inicialización **NO VERIFICADO**. |
| OSS UFunction `OnlineSubsystemMeteor.GetSteamAuthSessionTicket`, export #1621, cuerpo virtual `0x6e903`; hijo `ReturnValue:StrProperty`. EXE nombre nativo offset `0x168fcd8` / RVA `0x16908d8`, wrapper RVA `0xb73120`. | La declaración UPK enlaza por nombre/puntero con el wrapper, que llama directamente a `0xbc2130`; la cadena obtenida se codifica Base64url sin padding. | **CONFIRMADO** interfaz y cadena estática en [bindings](evidence/auth/upk-native-bindings.json) y [Ghidra](evidence/ghidra-main-upk-native-bindings.jsonl). Validación remota/vigencia **NO VERIFICADAS**; no se obtuvo un ticket real. |
| EXE error UTF16 offset `0x16a94c0` / RVA `0x16aa0c0`; referencia `0xbc22cc` en función `.pdata 0xbc2130..0xbc22fe`, recuperada en auth. | `Cannot retrieve authentication session ticket. User not logged in to Steam.` | **CONFIRMADO** literal y consumer estático; [protocolo de autenticación](HAWKEN_AUTH_PROTOCOL.md) conserva el productor y sus límites. No es prueba de un login ejecutado. |
| SDK ejecutores `0x417a0`, `0x42ea0`, `0x44680`, documentados en auth. | Login/create/link de Steam serializan `AuthTicket`; el login puede instalar accessGrant y cuenta Meteor. | **CONFIRMADO** flujo de construcción cliente; backend legítimo y asociación SteamID→MeteorGuid→perfil **NO VERIFICADO**. |

El parser UserInfo SDK `0x495f0` (offset `0x489f0`, lote anterior de results) lee opcionalmente `DeactivatedTime:string` (literal offset `0x1a3690` / RVA `0x1a4690`) y `Roles:array` de strings. **CONFIRMADO** metadatos del perfil; no equivale a una API de baneo, razón/duración de sanción ni revocación activa.

La cuenta de aplicación tiene al menos identidades distintas: Steam, Meteor `UserGuid`/grant y PlayFabId/SessionTicket. No se ha demostrado que sean intercambiables ni que sus identificadores coincidan. Para compatibilidad, el contrato futuro deberá mantener esa separación y vincularlos sólo tras una validación legítima; esto es una condición del plan, no una implementación existente.

## Datos persistentes y ajustes

| Ubicación | Observación estática | Interpretación / estado |
|---|---|---|
| OSS `OnlineRecentPlayersListMeteor.WritePersistentData`, export #2827, cuerpo virtual `0x81a74`, size443, tabla de export virtual `0x42fb0`. | Hijos `I`, `ZeroId`, `UsersList`, `TmpId`, `PlayerSettings`; la clase es la lista de recientes. `ReadPersistantData` #2814 virtual `0x81688`; `DecodeRecentPlayersJson` #2821 virtual `0x8195a`. | **CONFIRMADO** propietario y estructura. Persistencia de recientes **PROBABLE**; no prueba un recurso global `/persistentData` ni persistencia universal de stats/inventario. Bytecode no recuperado en este alcance. |
| OSS `ReadUserSettings` #1128 virtual `0x679f6`, `WriteUserSettings` #1143 `0x67d67`; callbacks #1132 `0x67acd`, #1145 `0x67de0`. | Interfaces con `SettingsCategory`, `SettingKeys`, `Settings`, `PlayerID`, éxito. `OnlinePlayerSettings` contiene getters/setters String/Float/Int/Bool y claves dinámicas (ledger). | **CONFIRMADO** interfaces locales. Vinculación precisa de categorías a proveedor, visibilidad y formato remoto **NO VERIFICADO**. |
| SDK builder `0x16910` (offset `0x15d10`), PlayFab `Client/UpdateUserData`. | Construye `Permission:"Public"`, objeto `Data` con clave dinámica→valor entero de entrada (JsonValue tipo1, asignación de `param_4`) y serializa JSON. | **CONFIRMADO** constructor. No debe clasificarse como ajustes privados por nombre; efectos reales/permisos **NO VERIFICADO**. |
| SDK builder `0x17ec0` (offset `0x172c0`), `Client/GetUserData`. | Construye `Keys` desde colección y añade `PlayFabId` si la cadena opcional no está vacía; resultado de clase `KeyValuePairCollection`. | **CONFIRMADO** construcción/identidad separada. Parser `0x17520` (offset `0x16920`) lee `data.Data` como object y para cada clave extrae `Value` a pares string. **CONFIRMADO** forma consumida; descarta otros metadatos de cada valor. Semántica completa de versiones/timestamps y autoridad **NO VERIFICADO**. |

La búsqueda de literales/exports del SDK no encontró `persistentData`; encontró estas APIs concretas y un `muc_persistent` de gloox sin vínculo a perfiles. Es una cobertura léxica del SDK, no prueba de ausencia funcional en EXE/paquetes.

## Estadísticas, inventario y progresión

| Función SDK / ubicación | Observación | Interpretación / estado |
|---|---|---|
| `0xb8ce0` / offset `0xb80e0`; `0xb9840` / `0xb8c40` | Construyen ruta `/userStats/`; JSON array de objetos con `Guid` y claves dinámicas de estadísticas, partiendo de `StringKeyDoubleValuePairCollection`. Las funciones usan modelos `UserStats` y resultado `ApiResult<void>`. | **CONFIRMADO** serialización cliente. Límite de nombres, reglas de incremento, saturación, transacción y permisos de escritura **NO VERIFICADO**. |
| `0x68990` / `0x67d90`; `0x69620` / `0x68a20` | Construyen `/userGameItemStats/`, array de objetos con `UserGuid`, `UserGameItemGuid` y claves de estadísticas dinámicas. | **CONFIRMADO** identidad cuenta+instancia y valores serializados; no confundir GUID de instancia con catálogo de tipo de mech. Autoridad/valores aceptados **NO VERIFICADO**. |
| `0x9fe0` / `0x93e0` | Ruta `/userGameItemsBroker/{escaped-id}/{escaped-id}`; construye objeto con claves dinámicas de `KeyValuePair`, serialización JSON. | **CONFIRMADO** constructor de actualización. Qué claves conceden posesión o cambian configuración, ownership y política **NO VERIFICADO**. |
| `0x657e0` / `0x64be0` | Construye `/userGameItems/{escaped-user-id}`, resultado `GameItemCollection`. | **CONFIRMADO** recuperación y parser GameItem descrito abajo. Catálogo auténtico, posesión, elegibilidad y efectos servidor **NO VERIFICADO**. |
| `/game-client/game-items`, `/gameItems/`, `/gameItemsBatch` (tabla de literales al final). | Rutas diferentes para datos de catálogo; `/userGameItems/` y `/userGameItemsBatch/` para instancias. | **CONFIRMADO** literales+xrefs. Separación catálogo/posesión **PROBABLE** y respaldada por nombres de modelos; catálogo completo y relaciones padres/hijos **NO VERIFICADO**. |
| `/statOverflow/`, `/statTransfer/`; OSS #1398 `SendToXPOverflow` virtual `0x6b61e`, #1413 `GetFromXPOverflow` `0x6b952`. | Interfaces con `Amount:IntProperty`, `MechId:StrProperty`, `bLegacyTransfer`; callbacks incluyen éxito. | **CONFIRMADO** superficies. Significado del pool, límite, coste y conversión histórica **NO VERIFICADO**; no adaptar la ruta heredada a una economía nueva sin contrato. |

En OSS, `OnlinePlayerStats` tiene valores iniciales, incrementos int/float, lecturas como string, setters condicionales mayor/menor y `MergeAllIncrements` (#2769, virtual `0x809d6`). Eso confirma un modelo local que acumula incrementos; no demuestra que el servidor acepte cualquier escritura del cliente ni proporciona fórmulas de XP.

Conexiones locales observables (OSS; todos **CONFIRMADO** como metadatos, uso real **NO VERIFICADO**):

| UFunction | Export / cuerpo virtual | Hijos relevantes |
|---|---|---|
| `ReadUserStats` | #1098 / `0x67367` | `PlayerID`, `PlayerStats`, `ReturnValue:int` |
| `WriteUserStats` | #1113 / `0x676aa` | `PlayerID`, `PlayFabId`, `PlayerStats`, `ReturnValue:int` |
| `IncrementUserGameItemStats` | #1202 / `0x68a54` | `PlayerID`, `Instance`, `Stats`, `ReturnValue:int` |
| `BatchIncrementUserGameItemStats` | #1217 / `0x68da0` | `StatsArray`, `ReturnValue:int` |
| `ReadGameItemInstanceCollection` | #1153 / `0x67fdc` | `PlayerID`, `ItemInstanceCollection` |
| `ReadUserGameItemStats` | #1232 / `0x690de` | `PlayerID`, `InstanceID`, `Stats`, `ReturnValue:int` |
| `ReadAllUserGameItemStats` | #1247 / `0x69426` | `PlayerID`, `InstanceCollection`, `ReturnValue:int` |

GAME incluye `R_Deathmatch.OnWriteUserStatsComplete` #10192 virtual `0x7e5bea`, `R_GFx_MechMenu_Base.ReadOnlinePlayerStats` #23346 `0x955b39`, sincronización de cuenta #23336 `0x95543e` y `R_MechSetup.OnBatchIncrementUserGameItemStatsComplete_WriteOnlineMechStats` #35742 `0xb34c0e`. **CONFIRMADO** puntos estructurales de gameplay/UI que consumen la superficie; flujo exacto, momento de guardado y autoría del incremento **NO VERIFICADO** sin bytecode/callers completos. No se infiere que el cliente sea fuente confiable del progreso.

## Contratos HTTP recuperados y transacciones de stats

Los siguientes tramos del [lote de contratos](evidence/ghidra-sdk-account-contracts.jsonl) heredan archivo/hash SDK. **CONFIRMADO** decompilación y método de construcción; autorización, recepción, almacenamiento e idempotencia del servicio **NO VERIFICADO**. `6e800` es el helper GET, `6e900` POST y `6ea20` PUT del WebClient; `6ec30` es llamada de método explícito y la cadena RVA `0x1a2348` / offset `0x1a1348` contiene `POST`.

| Ruta construida | Verbo / ejecutor RVA (offset original) | Body/resultado consumido |
|---|---|---|
| `/userStats/` | PUT `0xb7d80` (`0xb7180`) | Array de objetos por `Guid`, claves dinámicas numéricas y `TransactionId` añadido por ejecutor; resultado ApiResult<void> tras wrapper/estados. |
| `/userGameItemStats/` | PUT `0x679f0` (`0x66df0`) | Array por `UserGuid` y `UserGameItemGuid`, claves numéricas dinámicas, `TransactionId` añadido; ApiResult<void>. |
| `/userGameItemsBroker/{id}/{id}` | PUT `0x9de0` (`0x91e0`) | Objeto dinámico de pares serializados; ApiResult<void>. |
| `/userGameItems/{user}` | GET `0x652e0` (`0x646e0`) | Result se entrega a `0x60bc0`→`0x60d60`: raíz array, cada object pasa a `0x60730`; los elementos que el parser acepta se añaden a `GameItemCollection`. Arrays vacíos son aceptados; no-objects se saltan en este tramo. El parser GameItem acepta cualquier object, incluso vacío; campos mal tipados se ignoran según sus ramas. |
| `/userClientAchievements/{user}/{achievement}` | POST `0x2e8f0` (`0x2dcf0`) | Body array de IDs de instancias si se suministran; flag interno del helper `local_2618=1`; ApiResult<void>. |
| `/userDailyAssignments/{user}/{achievement}` | **GET** `0x307a0` (`0x2fba0`) | Result a parser UserAchievement `0x366d0`. El nombre de la operación de desbloqueo no cambia el verbo observable. |
| PlayFab `Client/UpdateUserData` | POST ejecutor compartido `0x11b50` (`0x10f50`), constructor tarea `0x16790` | `Permission:"Public"`, `Data:{clave:entero}`; éxito especial para HTTP200. |
| PlayFab `Client/GetUserData` | POST `0x17520` (`0x16920`), tarea `0x17120` | `Keys:[...]`, PlayFabId opcional; respuesta `data.Data:{clave:{Value:...}}` transformada a pares. |
| PlayFab `Client/WritePlayerEvent` | POST `0x11b50` (`0x10f50`), tarea `0x11840` | Wrapper `PlayFabId`, `EventName`, `Body` del apartado de telemetría; ApiResult<void>, no resultado de progreso. |

**Dependencia confirmada adicional:** antes del PUT de estadísticas, `0xb7d80`/`0x679f0` parsean el array, toman `Guid`/`UserGuid`, consultan helpers `0xd7b40`/`0xd7570` para obtener un string que añaden como `TransactionId` a cada objeto y serializan de nuevo. Tras HTTP2xx con estados internos no exitosos, consultan `0xd8d90` para IDs de transacción, con hasta cinco iteraciones en este tramo, y propagan un error si no se obtiene éxito. Esta ruta no se reduce a guardar el body original y devolver200: el cliente tiene un camino de seguimiento de transacciones. El [lote final](evidence/ghidra-sdk-account-final.jsonl) confirma `0xd7b40` (offset `0xd6f40`) como POST `/userGameTransaction/{escaped-user}` con Result JSONstring; `0xd7570` (offset `0xd6970`) construye la misma ruta para la variante asíncrona, ejecutor `0xd6f00` (offset `0xd6300`) POST y Result JSONstring. `0xd8d90` (offset `0xd8190`) construye consulta `/userGameTransaction/{escaped-user}/{escaped-transaction-id}`, ejecutor `0xd8650` (offset `0xd7a50`) **GET**, resultado modelo Transaction; el executor valida JSON y deriva el modelo a `0xd6880`, recuperado en el lote de modelos finales y descrito abajo. El [lote de parsers](evidence/ghidra-sdk-account-parsers.jsonl) corrobora ambos ejecutores. El literal está en offset `0x1a89a8` / RVA `0x1a99a8` y `TransactionId` en offset `0x1a4a10` / RVA `0x1a5a10`.

Esto confirma uso de transacciones en escritura de stats, **no** un pago, una moneda ni una wallet. El parser del resultado de seguimiento ya está recuperado: Status numérico representable como int32 y campos string opcionales, sin campos obligatorios. No se ha demostrado que el error genérico de compra represente moneda o pago. Cada petición debe respetar también el envelope Meteor documentado en [auth](HAWKEN_AUTH_PROTOCOL.md): Status/Message/Result y estados de transporte/resultados son capas separadas.

**Corrección y cierre estático 2026-10-11:** GET de transacción lleva **dos** segmentos, usuario y transacción; el cuerpo SDK RVA `0xd8d90` escapa y añade ambos. La descripción anterior omitía el usuario. El [contrato postmatch](../../implementation/evidence/postmatch-sync-contract/contract.json) conserva cuerpos, hashes y procedencia. Esta consulta aparece en el tratamiento de errores de PUT; no demuestra un bucle que espere `Transaction.Status` para cerrar SYNCING STATS. **CONFIRMADO estáticamente:** los parsers aceptan `Result:[]` para item stats y `Result:{}` para user stats sin mínimo de elementos; EXE RVA `0xbd1610` y `0xbd68f0`, SHA `4a861ecb4ac7add9554f8daf8f98ad2c12b9cb6c1865a83e8be808beb46ea8a2`, publican su estado de lectura completada en la rama de éxito. La emisión de callbacks pasa por la lista de delegados y el receptor activo, no por el número de estadísticas. **NO VERIFICADO:** qué receptor y estado asíncrono bloquearon la sesión real; HTTP200 por sí solo no prueba que ese callback se ejecutó.

## Parsers finales de inventario y transacciones

Fuente: [dos funciones finales](evidence/ghidra-sdk-final-models.jsonl), [contratos y tags corroborados](evidence/auth/final-model-contracts.json), [modelos](evidence/auth/account-models.json). Ambas filas conservan SHA SDK `f63fef02ffb517ca7844a769d03412ad82bd22198397302ee3c090c63a970c57`, ImageBase `0x180000000` y decompilación completada. **CONFIRMADO** controles y escrituras del consumidor estático; reglas del servicio, datos reales y autoridad **NO VERIFICADO**.

### GameItem — RVA `0x60730`, offset original `0x5fb30`

La raíz debe ser object (tag7); cualquier object, incluido `{}`, devuelve true. No hay un conjunto de campos obligatorios ni rechazo del elemento por un campo opcional ausente/mal tipado. Itera las propiedades; sólo procesa strings y el array específico descrito abajo. Esto refina el wrapper de colección: un elemento object vacío se añade, mientras un elemento no-object se omite.

| Clave / tipo observado | Escritura o tratamiento |
|---|---|
| `Guid:string`, `GameItemsGuid:string` | Ambos escriben el mismo slot `data+0`. No constituyen dos identidades independientes almacenadas por este parser. Si ambos se procesan, la última escritura sustituye la anterior; no se afirma el orden de recorrido. |
| `UserGuid:string` | Reconocido y salta al final de iteración sin escritura observable. No valida identidad o propiedad del inventario. |
| `UserGameItemGuid:string` | Escribe slot `data+0x20`. Mantenerlo separado del slot de Guid/GameItemsGuid. |
| `ItemName:string` | Llama a SetItemName. |
| `Version:string`, `GameItemsVersion:string` | Ambos llaman a SetVersion. Son aliases en este consumidor, sin prueba de dos versiones independientes. |
| `ReadOnly_Children:array` (tag6) | Itera elementos; únicamente strings llaman AddChildItemInstance. Son IDs string, no objetos GameItem embebidos ni recursión demostrada. Otros elementos se saltan. |
| Otras propiedades string | Se guardan como clave/valor en `KeyValuePairCollection` en `data+0x80`. No se impone una whitelist ni se determina su significado. Esto también cubre nombres especiales que caigan en la rama string sin coincidir con las claves string reconocidas. |
| Otros tipos | No se observa escritura en este parser. No convertir números/bools/objects desconocidos a propiedades string por inferencia. |

Los slots y aliases no demuestran cuáles IDs históricos eran de catálogo, instancia o dueño. Esa semántica requiere consumidores/datos adicionales. Que un object sea aceptado no significa que su inventario sea válido, poseído o utilizable por el jugador.

### Transaction — RVA `0xd6880`, offset original `0xd5c80`

La raíz debe ser object (tag7); `{}` es aceptado. Todos los campos son opcionales y el parser conserva el objeto aunque una propiedad tenga tipo no admitido.

| Clave | Tipo leído / slot relativo al modelo |
|---|---|
| `TransactionGuid` | string / `data+0` |
| `UserGuid` | string / `data+0x20` |
| `Created`, `Updated` | string / `data+0x40`, `data+0x60`; formato temporal no validado aquí |
| `Status` | número representable como int32 / `data+0x80` |
| `Message` | string / `data+0x88` |
| `Result` | **string** / `data+0xa8` |
| `UrlPath` | string / `data+0xc8` |
| `RequestBody` | string / `data+0xe8` |

`Transaction.Result` pertenece al object de transacción obtenido después de procesar el envelope Meteor HTTP. No debe confundirse con el `Result` del envelope ni enviarse automáticamente como object anidado: este parser sólo lee la variante string. `UrlPath` y `RequestBody` son nombres del schema original; no se capturaron cuerpos reales ni datos personales.

### Corroboración de tags y Status

Los tags observados coinciden con evidencia anterior:4=string (grant, UserAchievement/DeveloperData),6=array (root de `0x60d60` y Users del anuncio `0x84150`),7=object (raíces de modelos). La implementación interna del array puede recorrer una estructura de árbol; eso no convierte el tag6 de `ReadOnly_Children` en object.

El helper `0x5ab60` (offset `0x59f60`) acepta tag1 signed dentro de `[−2147483648,2147483647]`, tag2 unsigned hasta2147483647, y tag3 double dentro de ese rango cuya parte fraccionaria sea0. Usa `modf` importado de MSVCR110.dll en IAT RVA `0x18f770`; los límites double están en RVA `0x1ac728` / offset `0x1ab728` y RVA `0x1ac6f8` / offset `0x1ab6f8`, corroborados en bytes originales. Otros tags —incluidos bool/null/string— retornan false. El parser llama después a `0x59070` para extraer el entero. **CONFIRMADO** comprobación estática; significado de los códigos de estado y efecto de una transacción concreta **NO VERIFICADO**.

## Logros, asignaciones y recompensas

SDK `0x36ca0`, offset `0x360a0`, parser `Achievement`: raíz object, `AchievementGuid:string`, `Rewards:array` con elementos string aceptados, `UnlockFrequency` convertible a int, `Commencement:string`, `Expiration:string`, `Active:bool`, `Hackable:bool`, `DeveloperData:array` cuyos objetos pasan a un parser de DeveloperData, `CountryCode:string`, `Title:string`, `ImageId:string`, `LockedDescription:string`, `UnlockedDescription:string`, **`DisplayCategories:string`**, `DisplayUnits:string`. Es una cadena de comprobaciones para retornar true; `AssignmentType` se lee opcionalmente si convertible a int. **CONFIRMADO** tipos/controles del parser; el significado de frecuencia, fechas, flags y datos de desarrollador es **NO VERIFICADO**. `DisplayCategories` no debe enviarse como array sólo porque el modelo local sea StringCollection.

SDK `0x36b40`, offset `0x35f40`, parser `DeveloperData` ([lote de metadatos](evidence/ghidra-sdk-account-metadata.jsonl)): exige object con **`Name:string`, `Type:string`, `Value:string`**. Name literal RVA `0x1a391c` / offset `0x1a291c`; Type RVA `0x1a3914` / offset `0x1a2914`. **CONFIRMADO**. Aunque Type se llame numérico u otro nombre, Value sigue siendo string para este parser. Los nombres/valores concretos de condiciones no se recuperan de estas claves genéricas: no bastan para definir reglas ganadas.

SDK `0x36540`, offset `0x35940`, parser `UserAchievement`: raíz object y `UserGuid:string` requeridos; `AchievementGuid:string`, `DateEarned:string`, `AssignmentFlag` convertible a int se leen opcionalmente. **CONFIRMADO**. No exige aquí un Guid propio ni proof de condición; el significado de AssignmentFlag y el formato temporal siguen **NO VERIFICADO**. Se consume por el wrapper `0x366d0` y el GET diario `0x307a0`.

SDK `0x35dd0`, offset `0x351d0`, parser `AchievementReward`: raíz object y `Guid:string` exigidos. `GameOfferGuid:string` es opcional. Si `UserStatName` es string, exige `UserStatValue` convertible a int; si `UserGameItemStatName` es string, exige `UserGameItemStatValue` convertible a double; si `CurrencyName` es string, exige `CurrencyValue` convertible a int; `Error:string` es opcional. **CONFIRMADO**. Son alternativas representadas en un mismo modelo; no se ha demostrado el algoritmo que aplica todas las ramas ni una transacción de concesión. Nombres de setters de otras clases en pseudocódigo provienen de código compartido y no cambian las claves.

| Constructor / ubicación | Observación | Interpretación / estado |
|---|---|---|
| SDK `0x2eaf0`, offset `0x2def0` | `/userClientAchievements/{escaped-user}/{escaped-achievement}`. Si hay colección de IDs de game-items, serializa array de strings; hay camino de colección vacía. Resultado `ApiResult<void>`. | **CONFIRMADO** petición de desbloqueo cliente. Regla ganada, elegibilidad, verificación servidor e idempotencia **NO VERIFICADO**. No prueba permiso para autoconceder logros. |
| SDK `0x30c90`, offset `0x30090` | `/userDailyAssignments/{escaped-user}/{escaped-achievement}`, resultado `UserAchievement`. | **CONFIRMADO** construcción; día, calendario, reroll, límites y reclamación **NO VERIFICADO**. |
| OSS `UnlockAchievementObject` #1552 virtual `0x6d9ab`, callback #1557 `0x6daa2` | Identifica `PlayerID`, `AchievementGuid`, `TaskID` y `ErrorCode`. | **CONFIRMADO** interfaz y callback local; mapeo preciso al HTTP de arriba **PROBABLE** sin consumidor nativo completo. |
| OSS `ReadAchievementCollection` #1525 virtual `0x6d3a5`, `ReadUnlockedAchievementCollection` #1538 `0x6d68b` | Catálogo y logros adquiridos son objetos separados; CountryCode, Collection y PlayerID. | **CONFIRMADO** metadatos; catálogo/condiciones completas **NO VERIFICADO**. |
| OSS `ReadDailyCalendarRewards` #1834 virtual `0x717e9`, `RedeemDailyCalendarReward` #1837 `0x71893` | Callback de lectura #1778 `0x70a8d`; de reclamación #1795 `0x70e5d` con `LoginDay` y éxito. | **CONFIRMADO** superficies. No igualar automáticamente calendario de login con asignaciones diarias del SDK. |
| GAME `R_PlayerReplicationInfo.UnlockAchievement` #38622 virtual `0xbf1e03`; callback #38770 `0xbf728f`; `R_GameInfoBase` callback #4192 `0x750d5e` | Metadatos de logro y jugador en gameplay. | **CONFIRMADO** candidatos de integración; reglas comprobadas por servidor, orden y payload de red **NO VERIFICADO**. |

El paquete también conserva `R_CheatManager.UnlockAchievement` #13349 virtual `0x836b2c`. **CONFIRMADO** nombre/estructura de desarrollo; no es un camino autorizado para progreso ganado, ni prueba de accesibilidad en el producto distribuido. El plan debe basar premios en reglas de juego verificadas y registrar la decisión del servicio, no invocar esta interfaz como solución.

### Frontera de la economía histórica

Los originales contienen `/gameItemOffers` (literal RVA `0x1a6f40`), `/gameItemOffersFiltered` (`0x1a6f78`), `PurchaseGameOfferAsync::GetName` (`0x1a7040`), `/gameCurrency` (`0x1a7dd8`), `/meteorCurrency` (`0x1a7e28`) y getters de balances. **CONFIRMADO** literales/exports; no demuestra que un usuario tenga saldo ni que el servicio exista. EXE importa `GetCurrencyName/Value@AchievementReward` y `GetGameOfferId@AchievementReward` junto con getters de recompensas de estadísticas (PE linkage). El error `The purchase operation succeeded, but acquiring the new inventory failed.` tiene xref SDK `0x8f1d0`, literal RVA `0x1a6ff0`: la compra histórica y refresco del inventario tenían etapas separadas.

No se ha demostrado que `UnlockClientAchievement` dependa de saldo. El parser de recompensa permite omitir campos de moneda; eso hace viable como hipótesis definir recompensas directas de estadísticas/items, pero **NO VERIFICA** la compatibilidad de todos los menús. Tampoco demuestra que `GameOfferGuid` conceda un item sin transacción. La revisión permite saldo y canjes de créditos ganados jugando; deben contrastarse sus consumers/UX y persistencia, sin atribuir autoridad al parser. Las ramas premium/pagos quedan excluidas y no se conoce todavía el estado correcto para todos sus menús.

La cadena genérica `Purchase request failed... TransactionId(%s)` tiene xrefs a resolvers de stats y a compra: su reutilización no acredita que escribir stats sea una compra ni que requiera pago.

## Tres categorías diferentes de eventos

### 1. Callbacks locales y delegates de Unreal

OSS `OnReadUserStatsComplete` #1103 virtual `0x6745e`, `OnWriteUserStatsComplete` #1118 `0x677a6`, `OnBatchIncrementUserGameItemStatsComplete` #1222 `0x68e92`, `OnStatisticChanged` #1720 `0x6fce3`, `OnUnlockAchievementObjectCompleted` #1557 `0x6daa2` y los de inventario arriba: **CONFIRMADO** UFunctions/delegates. Los callbacks de stats transportan objetos locales, PlayerID, TaskID y ErrorCode; `OnStatisticChanged` tiene `PlayerNetId`, `StatName:NameProperty`, `NewStatValue:StrProperty`.

**NO VERIFICADO** que estos delegates se publiquen por red. Los nombres `On...Complete` informan terminación local de tarea y no deben documentarse como un bus público ni como prueba durable de progreso confirmado.

### 2. Eventos de gameplay

GAME conserva `R_EventTracking.SendEvent_MatchCreated` #23978 virtual `0x97abc3`, `SendEvent_MatchStarted` #24004 `0x97b81c`, `SendEvent_MatchEnded` #24022 `0x97c089`, `SendEvent_PlayerSpawned` #24090 `0x97e7d7`, `SendEvent_PlayerKill` #24137 `0x98146d`, `SendEvent_PlayerSustainedDamage` #24167 `0x9827e2`, `SendEvent_MatchEndMechStatus` #24239 `0x9840a7`. **CONFIRMADO** UFunctions y outer del paquete; sus propiedades y análisis están en [protocolo de juego](HAWKEN_GAME_SERVER_PROTOCOL.md). **NO VERIFICADO** contrato de red, emisión real, dirección cliente/servidor, confiabilidad o autoridad histórica.

No todos los eventos de gameplay deben ser externos. Estadísticas de daño por frame, daño bruto y configuración completa pueden tener volumen y sensibilidad diferentes de un resultado final de partida. Esa selección es una decisión futura del plan, no un hecho recuperado.

### 3. Telemetría de servicios

| Ubicación SDK | Observación | Estado y límites |
|---|---|---|
| Constructor `0x4960`, offset `0x3d60` | Inicializa EventConsumer, ruta `/gameClientEvent`, registra nombres `player_Started`, `player_Finished`, `player_MechConfigurationSaved`, `match_PeriodicMatchConcurrencyUpdate`, `match_Started`, `match_Ended`. | **CONFIRMADO** registro de nombres y ruta. No demuestra whitelist exhaustiva, recepción ni que la ruta envíe cada evento. |
| Builder `0x11f10`, offset `0x11310` | Construye PlayFab `Client/WritePlayerEvent`, wrapper JSON `PlayFabId`, `EventName`, `Body`; Body incluye `producerId`, `producerType`, `subjectId`, `subjectType`, `Verb`, `targetId`/`targetType` condicionales, `EventParams` si el JSON de EventData es válido, `userID`, `sessionID`, `eventName`. | **CONFIRMADO** construcción y casing; origen/validez de IDs, semántica exacta de Verb y aceptación real **NO VERIFICADO**. `EventParams` mayúscula y `eventParams` minúscula de otros builders son contratos distintos. |
| `EventData.AddData` export RVA `0x50d40`; GetDataString `0x50e90` | Colección de datos de evento en el cliente. | **CONFIRMADO** exports; tipos completos y sanitización **NO VERIFICADO**. |
| Sender DeltaDNA `0x53e60`, offset `0x53260`; batching `0x53d80`, literal `{"eventList":[` RVA `0x1a4ba0` | URL construida `https://collect5957hwknp.deltadna.net/collect/api/{valor-consumer}/bulk`, cola y umbral de batch de 2MB; rama de envío separada. | **CONFIRMADO** construcción estática recuperada en lote auxiliar. No prueba recepción, reintento durable, consentimientos ni equivalencia con PlayFab/Meteor. |

No se ha demostrado que la telemetría histórica conceda XP/recompensas ni que exista una salida webhook/SSE/WebSocket lista para administradores. Debe ser una entrada observada, con validación y procedencia conocidas antes de usarla en decisiones de progreso o sanción.

## XP, controles de host y transporte de eventos recuperados en UPK

La ampliación usa los archivos/hashes EXE, SDK y GAME de la tabla de identidad; las posiciones de paquete son virtuales. **CONFIRMADO estático** se refiere al cuerpo concreto, sin afirmar recepción remota, autoridad backend o persistencia SQL. [Funciones revisadas](evidence/game/upk-reviewed-functions.json), [bindings originales](evidence/game/upk-native-bindings.json) y [protocolo de partida](HAWKEN_GAME_SERVER_PROTOCOL.md) conservan flags, offsets, bloques fuente y hashes de outputs.

| Fuente / localización | Evidencia observada | Interpretación y límite |
|---|---|---|
| GAME AwardXP, export14585 / `0x856516`; EXE ScorePoints `0xdaea90` | AwardXP redondea `fracción*TotalExpPoints` con FFloor(+0.5). ScorePoints aplica global/modo/antiadicción/partes de mech y convierte float a int; sin multiplicadores, factores1. | CONFIRMADO fórmula local parcial. Getters de factores, balance completo, tiers y concesión durable NO VERIFICADOS. |
| EXE `0xd89c40` / `0xd89550` / `0xd898f0` | Incrementa contadores de jugador/mech y deltas ExpPoints/EarnedExpPoints; hay guards de servidor/stats y notificaciones. GAME receptores XP exports38746/38756 declaran reliable client simulated. | CONFIRMADO mutación/notificación local; no prueba commit de perfil/inventario SQL ni serialización completa RPC. |
| GAME GainedXP / MechGainedXP, exports24224/24232, `0x983dbf`/`0x983f5b`; EXE `0xdb1d60`/`0xdb23b0` | Builders nativos incluyen MatchID, XPAmount, GainReason, IsSkillXP; mech añade IDs/tipos bajo guards. | CONFIRMADO objetos locales y su cadena de envío; cliente telemétrico no autoriza premios. |
| EXE `0xb93ec0` → `0xbe0e10` → `0xbad7b0`; SDK `0x4d70` → `0x51740` | Convierte EventObject, valida strings, agrega fechas y encola bajo mutex; devuelve 202 local de aceptación diferida. | CONFIRMADO paso hacia cola de telemetría; no ack remoto ni validación de una identidad Steam. |
| SDK worker `0x515f0`/`0x51fd0` → `0x526a0` → `0x52560` → `0x6e900` | Intervalo5000ms; POST `/gameClientEvent`, raíz array JSON con Producer/Subject, Verb, Timestamp, Target/Data opcionales. Cola drenada antes de enviar; DeltaDNA_ se deriva por separado. | CONFIRMADO transporte estático en [event-queue-transport.json](evidence/auth/event-queue-transport.json). Reintento durable/ack por evento y recepción real NO VERIFICADOS; no es contrato de otorgamiento de XP. |
| GAME ServerKickBan15678 / `0x86e7c3`, AdminSay15682 / `0x86e96c`, ServerChangeMap15690 / `0x86ecd3` | Guards IsServerAdmin y, para KickBan, alternativa LocalPlayer; AdminSay exec hace broadcast; ServerChangeMap exige admin antes de travel. | CONFIRMADO flags/controles de los cuerpos. Un panel nuevo necesita autenticación/alcance propios y prueba permitida/denegada; no inferir permiso por nombre ni ownership SQL. |

Las categorías settings `Game=0`, `Web=1`, `Meteor=2`, `PublicReadOnly=3` y el uso `Player.RecentPlayers` para categoría0 quedaron recuperados en [auth](HAWKEN_AUTH_PROTOCOL.md). Guardar jugadores recientes en settings no acredita un historial universal de visitas o progreso; éstos son datos nuevos del backend futuro.

Los paquetes adicionales tienen sus hashes por entrada en [upk-additional-reviewed-functions.json](evidence/game/upk-additional-reviewed-functions.json). En `HawkenGame/CookedPCConsole/HawkenSG_SF.upk`, SHA-256 `4683bd667318367c12c004626fa5bbe8d4fde21aa06652bb669f927a525cec7d`, `R_SiegeTurret.SendOutDamageXPOnDeath` export74 / `0x3cd02` sólo exige RoleAuthority, marca el flag y llama DropEU/SpawnPickup: **no concede XP en ese cuerpo**. El tutorial `HawkenGame/CookedPCConsole/HawkenTR_SF.upk`, SHA-256 `3e0eb2030ec7a1c465800a98023df16d02b0a1d35c47fdb6ca45bd02ebc46c1c`, export34 / `0x20dc2` llama IncrementConsoleStat con TutorialComplete/GameProgress; persistencia remota NO VERIFICADA. No exponer nombres de función como hechos de recompensa sin comprobar su cuerpo y origen.

## Candidatos a exponer en el plan

Estos nombres son **propuestas del proyecto**, no nombres de endpoints/eventos recuperados. Los estados reflejan las fuentes existentes; publicar un evento propio requerirá el resultado durable del servicio y permisos definidos.

| Candidato | Fuente estática y estado | Datos disponibles / frontera por resolver |
|---|---|---|
| Cuenta autenticada / sesión terminada | Login/grant/logout SDK, auth doc: **CONFIRMADO** cliente; verificación Steam **NO VERIFICADO**. | Identidades verificadas y sesión del servicio; nunca grant, ticket ni SessionKey en payload público. |
| Estadísticas confirmadas | REST stats y OnWriteUserStatsComplete: **CONFIRMADO** superficies; commit **NO VERIFICADO**. | UserGuid, itemGuid cuando aplica, claves y valores/deltas; añadir identificación de partida/origen e idempotencia será contrato propio. Callback local no acredita commit SQL. |
| XP observada / notificada | ScorePoints, contadores, RPC declaradas y eventos XP→cola→POST: **CONFIRMADO** cadena estática; premio durable **NO VERIFICADO**. | MatchID, XPAmount, GainReason y IDs de mech bajo guards. Para emitir progreso confirmado se requiere resultado de servidor autorizado y commit SQL; telemetría queda como diagnóstico. |
| Item desbloqueado / inventario cambiado | userGameItems + broker + lecturas locales: **CONFIRMADO** superficies; concesión por jugar **NO VERIFICADO**. | Identidad de instancia/tipo y causa de concesión, con fuente de resultado autorizado; no originar concesión por mensaje de UI ni compra heredada. |
| Logro / objetivo completado | UnlockAchievementObject, ClientAchievement y reward parser: **CONFIRMADO**; reglas **NO VERIFICADO**. | AchievementGuid, usuario, instancia opcional, recompensas de stats/items, fecha y condición satisfecha tras validación del servicio. |
| Partida creada/iniciada/finalizada | R_EventTracking y EventConsumer: **CONFIRMADO** nombres/interfaces. | ID de partida/servidor y resultado final confiable; schemas y autoridad requieren trabajo adicional. |
| Jugador entra/sale / actividad de cuenta | PlayerSpawned / eventos Started/Finished: **CONFIRMADO** nombres, interpretación **PROBABLE**. | No igualar spawn con login ni Started con admisión; usar sesión/admisión demostrada y definir límites. |
| Baneo/revocación/auditoría admin | Mandato del usuario; **NO VERIFICADO** API histórica para baneos en este alcance. | Contrato nuevo con autor, cuenta, razón, alcance, duración y consecuencia de sesión; no inferirlo de la telemetría. |
| Comando/mensaje administrativo personalizado | Mandato del usuario; **NO VERIFICADO** canal arbitrario existente. | Dirección, permisos, catálogo de comandos, límites y confirmación; no insertar comandos remotos en un evento de telemetría sin consumidor demostrado. |

Para desbloqueos ganados, faltan factores/balance completos de XP y tiers, catálogo/condiciones de logros y items, fuentes de resultado de partida, adjudicación fiable por servidor, protección contra repetición, comportamiento offline, reconciliación y errores. La fórmula local parcial anterior no resuelve esas reglas durables. SQL/Linux son decisiones del proyecto: ningún hallazgo demuestra qué base de datos usaba el backend original o que estas DLL Windows se ejecuten en Linux.

## Cobertura y pendientes de compatibilidad

Se recuperaron 43 funciones de los seis lotes SDK de cuentas/eventos, contratos y modelos con decompilación completada y hash cotejado, más lotes anteriores y metadatos package/imports. La extracción no equivale a todo el backend: no hay datos de servicio históricos, schemas completos de cada ruta, todos los consumidores EXE ni prueba runtime. El ledger contiene candidatos con bytes/xrefs y separa exports de ejecución.

Pendientes concretos: datos auténticos y semántica de Transaction/GameItem; autorización remota de categorías/settings; DeveloperData de condiciones, getters de factores XP y tiers; adjudicación final de recompensas y branches de wallet/canjes; serialización/dispatch completo de RPC y autoridad efectiva; motor de eventos servidor; confirmación backend Steam legítima. Los cuerpos/flags seleccionados ya recuperados se describen arriba. El proyecto no puede declarar fidelidad de progresión, saldo durable o compatibilidad sin premium/pagos sólo a partir de nombres y tipos.

## Ampliación Creator: créditos ganados, reglas y tarea de wallet

[creator-progression-findings.json](evidence/auth/creator-progression-findings.json) conserva 63 hallazgos, 32 exports propios positivos cotejados, coordenadas/bloques/hash originales, hashes de .uc, bindings y cuerpos nativos. Heredan identidades GAME/OSS/EXE/SDK de la tabla inicial; offsets UPK virtuales. El [diseño del Creator](HAWKEN_SERVER_CREATOR_DESIGN.md) utiliza esta evidencia, sin implementar recompensas.

| Función/localización | Evidencia e interpretación / estado |
|---|---|
| GAME AwardEndofGameHawkenPoints #10128, virtual `0x7e3b06`; CoOp #13992, `0x847828` | Tiempo, win/loss y oleadas alimentan IncrementPlayerHP. DM excluye bots/jugadores sin entrada; CoOp tiene condiciones propias. **CONFIRMADO** cálculo script; orden efectivo/autoridad remota/commit **NO VERIFICADOS**. |
| GAME IncrementPlayerHP #38615, `0xbf18ec`; GetPointsForTypeHP #4613, `0x75e543`; GetPointsHP #4617, `0x75e65c` | Lee GlobalMultiplierHP, quickmatch/antiadicción/mech y tablas HawkenPoints; incrementa contador/stat y envía GainedHP. **CONFIRMADO** cuerpo; no se recuperó una API Creator para modificar tablas ni concesión durable por esa mutación. |
| GAME WriteStats #10231, `0x7e6407`; GiveEndOfGameXPAwards #10063, `0x7e1861` | WriteStats retorna por bModifiedGameRules antes de premios; GiveEndOfGameXPAwards exige ROLE_Authority. **CONFIRMADO** guards estáticos; no prometer un evento que paga por alterar un multiplicador. |
| GAME MP.InitGame #9609, `0x7d2d1a`; ServerForceEndMatch #15801, `0x8723bf` | Opción bModifiedGameRules y final forzado admin que marca true y advierte de HC/XP sin premios normales. **CONFIRMADO** setters; no falsificar el flag. |
| OSS ConvertOfferCurrencyToStorm #1318, `0x6a49c`; EXE `0xb70f40`→`0xbae1e0`, strings RVA `0x16ab3d4`/`0x16ab464` | Enum0→MP, enum1→HP, otros vacío. **CONFIRMADO** enum/string; no denominar HP y MP como la misma moneda ni activar la premium. |
| OSS GiveWalletCurrency #1428, `0x6bc93`; EXE `0xb718c0`→vtable `0x169c610`+`0x558`→`0xbc23e0`→ctor `0xba0710`, task vt `0x16bc570`+`0x30`→`0xbe4400` | Captura PlayerID/currency/amount, pero tick sólo marca estado y tiempo. **CONFIRMADO** cuerpo; tarea local/stub **PROBABLE**, grant remoto/persistencia **NO VERIFICADOS**. Una devolución o callback no significa saldo concedido. |
| OSS RefreshWallet #1375, `0x6b0ec`; EXE `0xb715f0`→`0xbdcaa0`→`0xba1c40`, task `0xbe7770` | Dos interfaces de balance, estados separados y contadores de hasta5 intentos. **CONFIRMADO** ruta distinta de Give; mapeo exacto core+`0x290` a rutas SDK y frescura/commit backend **NO VERIFICADOS** en este slice. |
| GAME WritePlayerOnlineStats #10187, `0x7e5af8`; OnGiveWalletCurrencyComplete #10198, `0x7e5d13`; wrappers EXE `0xd1f040`/`0xd1f550` | Wrappers despachan virtuales +`0x7c0`/+`0x7d0`; destinos reales no recuperados. **CONFIRMADO** binding/dispatch, **NO VERIFICADO** cuerpo efectivo Game y adjudicación final. |

Las [funciones nuevas](evidence/ghidra-main-creator-functions.jsonl), [destinos/mapping](evidence/ghidra-main-creator-credit.jsonl), [constructores](evidence/ghidra-main-creator-wallet-task.jsonl), [métodos](evidence/ghidra-main-creator-wallet-method.jsonl) y [vtable original](evidence/creator-task-vtables.json) conservan SHA del EXE, completitud y bytes de los slots. La telemetría GainedHP reutiliza la cola/POST recuperados: aceptación local202 y POST no acreditan saldo, SQL ni permiso de host.

Diseño nuevo: el servicio central de cuentas/progreso bajo el operador Master conserva rates, reglas/version, resultados de GameServers autorizados y ledger/saldo/canje/grants/outbox transaccionales e idempotencia. Creator consulta/propone eventos sin autoridad de emisión global; particulares con progreso local sólo en un realm aislado. Cosméticos antes vendidos requieren concesión, persistencia y equipamiento compatibles por objeto. Compatibilidad UI sin MP/pagos, factores efectivos y condición de reglas modificadas son gates pendientes. No se ejecutó el cliente ni se obtuvo un saldo/ticket real.

## Congelado de esta entrega

La [validación local anterior](evidence/auth/account-validation.json) cotejó hashes SDK/EXE/AppID, literales, lotes de decompilación y exports/hijos de paquetes para su selección. La ampliación posterior añade bytecode/bindings/cola/POST con sus ledgers y controles específicos; el [control final](evidence/final-research-validation.json) registra los hashes actuales de los nueve informes e integridad del juego. Estas comprobaciones validan procedencia y consistencia, no el protocolo en ejecución. No se registró información personal de usuarios ni tickets/grants reales. Permisos Steam, catálogo histórico, balance/reglas durables de progreso, semántica/valores auténticos de items/transacciones y compatibilidad completa de UI sin economía conservan el estado pendiente.

## Anexo: rutas y literales con coordenadas originales

Tabla SDK extraída de bytes, con xrefs `.pdata` del ledger. **CONFIRMADO** literal y xref cuando se indica; **NO VERIFICADO** uso runtime. Los verbos sólo se afirman en las secciones recuperadas, no por el nombre de ruta.

| Literal SDK | Offset original | RVA literal | Funciones xref RVA |
|---|---|---|---|
| `/gameClientEvent` | `0x1a0680` | `0x1a1680` | `0x4960` |
| `eventName` | `0x1a0770` | `0x1a1770` | `0x11f10`, `0x57c0`, `0x5e80`, `0x6460`, `0x68b0`, `0x6d00`, `0x76d0` |
| `eventTimestamp` | `0x1a07a8` | `0x1a17a8` | `0x57c0`, `0x5e80`, `0x6460`, `0x68b0`, `0x6d00`, `0x76d0` |
| `eventParams` | `0x1a07f8` | `0x1a17f8` | `0x57c0`, `0x5e80`, `0x6460`, `0x68b0`, `0x6d00`, `0x76d0` |
| `/userGameItemsBroker/` | `0x1a0a70` | `0x1a1a70` | `0x9fe0` |
| `Client/WritePlayerEvent` | `0x1a1538` | `0x1a2538` | `0x11f10` |
| `Client/UpdateUserData` | `0x1a16d8` | `0x1a26d8` | `0x16910` |
| `Client/GetUserData` | `0x1a1718` | `0x1a2718` | `0x17ec0` |
| `/achievements` | `0x1a24c0` | `0x1a34c0` | `0x2a2e0`, `0x2c360` |
| `/achievementsBatch` | `0x1a24e0` | `0x1a34e0` | `0x2b490` |
| `/userAchievements` | `0x1a2588` | `0x1a3588` | `0x2c880` |
| `/userAchievementsQuery/` | `0x1a25a0` | `0x1a35a0` | `0x2d920` |
| `/userClientAchievements` | `0x1a2600` | `0x1a3600` | `0x2eaf0` |
| `/achievementReward/` | `0x1a2700` | `0x1a3700` | `0x32c20`, `0x33360` |
| `/achievementRewardBatch/` | `0x1a27b8` | `0x1a37b8` | `0x340c0` |
| `/game-client/achievement-rewards` | `0x1a2800` | `0x1a3800` | `0x34c50` |
| `/game-client/achievements/` | `0x1a2830` | `0x1a3830` | `0x35950` |
| `Rewards` | `0x1a2938` | `0x1a3938` | `0x36ca0` |
| `/gameItems/` | `0x1a4920` | `0x1a5920` | `0x63350` |
| `/gameItemsBatch` | `0x1a4958` | `0x1a5958` | `0x647f0` |
| `/userGameItems/` | `0x1a4988` | `0x1a5988` | `0x657e0`, `0x665b0` |
| `/userGameItemsBatch/` | `0x1a49f8` | `0x1a59f8` | `0x66e60` |
| `/userGameItemStats/` | `0x1a4ac0` | `0x1a5ac0` | `0x68990`, `0x69620`, `0x6ac10`, `0x6bc20` |
| `/statOverflow/` | `0x1a7548` | `0x1a8548` | `0xb2f30` |
| `/statTransfer/` | `0x1a75a8` | `0x1a85a8` | `0xb3670`, `0xb45e0` |
| `/userStats/` | `0x1a78c0` | `0x1a88c0` | `0xb8ce0`, `0xb9840`, `0xbadc0`, `0xbc200`, `0xbd000` |
| `/userStats` | `0x1a7948` | `0x1a8948` | `0xbc4f0` |
