# HAWKEN — inventario binario y cobertura

Fecha de investigación: 2026-10-10. Original: `F:\SteamLibrary\steamapps\common\Hawken`. Resultados: `F:\HawkenServer\research\2026-10-10`, fuera del juego. Investigación estática: ningún ejecutable o DLL del juego fue ejecutado, parcheado ni cargado como código.

## Inventario completo

Se enumeraron y calcularon SHA-256 de **946 archivos**, **3,655,803,216 bytes**. Errores de enumeración/hash: **0**. El catálogo completo, incluidas rutas absolutas/relativas, tamaño, fecha UTC y versiones presentes, está en [inventory.csv](evidence/inventory.csv); los totales y errores completos están en [inventory-summary.json](evidence/inventory-summary.json). Las fechas de archivo no se interpretan como fechas de compilación.

| Tipo | Cantidad | Cobertura |
|---|---:|---|
| EXE | 3 | Hash, recursos de versión, PE/imports/exports/delay imports; análisis nativo selectivo del cliente y managed del reporter. |
| DLL | 83 | Hash, versiones presentes y PE completo; análisis selectivo de Meteor y networking de terceros. |
| INI | 29 | Hash y búsqueda textual; se distinguieron defaults de UE3 de overrides propios de Hawken. |
| UPK | 360 | Hash, tablas y barrido ASCII/UTF-16 del contenido descomprimido de todos; extracción de clases/funciones propias y revisión selectiva de consumidores, con límites explícitos. |
| BIN | 4 | Hash de todos y cabeceras; tres GlobalShaderCache y PersistentCookerShaderData. No hay Coalesced.bin en el inventario. |
| Otros | 467 | Hash y metadatos de todo el árbol; no se atribuye networking a texturas, vídeos o localización por su nombre. |

## Identificación verificada de motor y cliente

**INV-001 — CONFIRMADO (recursos PE/configuración/paquetes).** El cliente `Binaries\Win64\HawkenGame-Win64-Shipping.exe` es PE32+ x86-64, ImageBase `0x140000000`, versión de archivo/producto literal `1, 6, 0, 143417`, producto `Hawken`, compañía `Reloaded Games, Inc.`. SHA-256 `4a861ecb4ac7add9554f8daf8f98ad2c12b9cb6c1865a83e8be808beb46ea8a2`. `AutoReporter.exe` declara producto `UnrealEngine3`; las configuraciones usan clases `Engine`, `IpDrv` y `OnlineSubsystemMeteor`. Los paquetes contienen tablas y tipos Unreal compatibles con UE3. La combinación confirma UE3 como motor de esta muestra; no identifica una rama completa del código fuente ni prueba equivalencia con UDK.

**INV-002 — CONFIRMADO (selección configurada).** `HawkenGame\Config\DefaultEngine.ini`, SHA-256 `6f839d73d16f512fbbf29e4d35ea12ba1e8604c8372ea72cad90dabff2526599`, línea 143: `DefaultOnlineSubsystemName=OnlineSubsystemMeteor.OnlineSubsystemMeteor`. `Binaries\Win64\StormConsumerSettings.ini`, SHA-256 `08eda2cb20539996b7c1a91480c1ee2f64e02743c9bbee0bfc4f3543d8fec3d9`, línea 4: URI base Meteor. Selección de configuración no significa ejecución observada.

## Importaciones, exportaciones y módulos vinculados

[pe-linkage.json](evidence/pe-linkage.json) decodifica **86/86 PE**, **383 descriptores de import normal**, **16 de import diferido** y **60,558 entradas exportadas**. Retiene cada nombre/ordinal, IAT RVA, descriptor, forwarder, secciones, base, entrypoint, directorio CLI, CodeView/PDB cuando está presente y cantidad de registros `.pdata`. Un descriptor vincula módulos declarados; no acredita que el cargador los haya usado en esta sesión.

Las salidas originales de `objdump -p` están en [pe-objdump](evidence/pe-objdump), con [códigos de salida](evidence/pe-objdump-exit-codes.csv). 85/86 fueron 0; `libsndfile_x64-1.dll` fue rechazado por esa herramienta. El parser PE independiente sí leyó su cabecera, imports y exports; el rechazo no se oculta ni se interpreta como ausencia de PE.

[pe-cross-validation.json](evidence/pe-cross-validation.json) contrasta las 85 salidas objdump exitosas con el parser: arquitectura, base, módulos importados y cantidad de nombres exportados concuerdan. Las cabeceras MZ/PE de los 86 archivos son válidas, incluida libsndfile. El cruce no verifica ejecución del cargador ni cada rama de código.

| Origen | Dependencia declarada | Evidencia e interpretación | Estado |
|---|---|---|---|
| `HawkenGame-Win64-Shipping.exe` | `Meteor.Sdk.Cpp.Win32.Client64.dll` | Descriptor RVA `0x1ad3080`; ??0ApiResultBase@MeteorSdk@@QEAA@XZ, ??1ApiResultBase@MeteorSdk@@UEAA@XZ, ?GetCurrencyValue@AchievementReward@MeteorSdk@@QEBAHXZ, ?GetBatchError@AchievementReward@MeteorSdk@@QEBAPEBDXZ, ??0AchievementRewardCollection@MeteorSdk@@QEAA@XZ; 341 símbolos completos en JSON. | CONFIRMADO (PE) |
| `HawkenGame-Win64-Shipping.exe` | `steam_api64.dll` | Descriptor RVA `0x1ad3094`; SteamAPI_RestartAppIfNecessary, SteamAPI_Shutdown, SteamAPI_Init, SteamGameServer_RunCallbacks, SteamAPI_RunCallbacks; 15 símbolos completos en JSON. | CONFIRMADO (PE) |
| `HawkenGame-Win64-Shipping.exe` | `WININET.dll` | Descriptor RVA `0x1ad30d0`; InternetSetOptionW, InternetReadFile, InternetGetLastResponseInfoW, InternetSetStatusCallbackW, HttpOpenRequestW; 14 símbolos completos en JSON. | CONFIRMADO (PE) |
| `HawkenGame-Win64-Shipping.exe` | `WSOCK32.dll` | Descriptor RVA `0x1ad3170`; ordinal 19, ordinal 18, ordinal 2, ordinal 17, ordinal 16; 25 símbolos completos en JSON. | CONFIRMADO (PE) |
| `HawkenGame-Win64-Shipping.exe` | `IPHLPAPI.DLL` | Descriptor RVA `0x1ad3274`; GetAdaptersInfo. | CONFIRMADO (PE) |
| `Meteor.Sdk.Cpp.Win32.Client64.dll` | `LIBEAY32.dll` | Descriptor RVA `0x21eab4`; ordinal 391, ordinal 542, ordinal 544, ordinal 1958, ordinal 566; 65 símbolos completos en JSON. | CONFIRMADO (PE) |
| `Meteor.Sdk.Cpp.Win32.Client64.dll` | `SSLEAY32.dll` | Descriptor RVA `0x21eac8`; ordinal 157, ordinal 242, ordinal 141, ordinal 126, ordinal 49; 39 símbolos completos en JSON. | CONFIRMADO (PE) |
| `Meteor.Sdk.Cpp.Win32.Client64.dll` | `WS2_32.dll` | Descriptor RVA `0x21eb18`; ordinal 5, ordinal 57, ordinal 10, ordinal 115, ordinal 116; 29 símbolos completos en JSON. | CONFIRMADO (PE) |
| `Meteor.Sdk.Cpp.Win32.Client64.dll` | `vivoxsdk_x64.dll` | Descriptor RVA `0x21eb40`; vx_req_aux_get_mic_level_create, destroy_evt, vx_copy_audioBuffer, destroy_resp, vx_event_to_xml; 28 símbolos completos en JSON. | CONFIRMADO (PE) |
| `Meteor.Sdk.Cpp.Win32.Client64.dll` | `WLDAP32.dll` | Descriptor RVA `0x21eb54`; ordinal 46, ordinal 211, ordinal 60, ordinal 50, ordinal 41; 16 símbolos completos en JSON. | CONFIRMADO (PE) |
| `Meteor.Sdk.Cpp.Win32.Client64.dll` | `WINHTTP.dll` | Descriptor RVA `0x21eb90`; WinHttpOpenRequest, WinHttpOpen, WinHttpQueryDataAvailable, WinHttpQueryHeaders, WinHttpCloseHandle; 10 símbolos completos en JSON. | CONFIRMADO (PE) |
| `Meteor.Sdk.Cpp.Win32.Client64.dll` | `DNSAPI.dll` | Descriptor RVA `0x21eba4`; DnsFree, DnsQuery_UTF8. | CONFIRMADO (PE) |
| `Meteor.Sdk.Cpp.Win32.Client64.dll` | `Secur32.dll` | Descriptor RVA `0x21ebb8`; InitializeSecurityContextA, FreeCredentialsHandle, AcquireCredentialsHandleA, QueryContextAttributesA, FreeContextBuffer; 8 símbolos completos en JSON. | CONFIRMADO (PE) |
| `Meteor.Sdk.Cpp.Win32.Client64.dll` | `CRYPT32.dll` | Descriptor RVA `0x21ebcc`; CertFreeCertificateChain, CertGetCertificateChain, CertNameToStrA, CertVerifyCertificateChainPolicy. | CONFIRMADO (PE) |

**INV-003 — CONFIRMADO (frontera nativa).** El cliente importa 341 funciones de la DLL Meteor; su módulo contiene APIs de autenticación/servicios y usa WinHTTP. El EXE importa WSOCK32 y contiene networking UE3. Esta separación de importaciones y funciones es evidencia de componentes distintos; no demuestra un Master Server independiente en producción.

## Componentes de terceros

| Componente | Archivo/evidencia local | Interpretación y límite | Estado |
|---|---|---|---|
| Meteor/Storm | Meteor DLL; exports MeteorSdk/MeteorCore, WinHTTP, configuración MeteorUri | Backend personalizado, cuenta, inventario, matchmaking, presencia y consumidores de terceros. | CONFIRMADO interfaces/código estático |
| Steam | steam_api64.dll; EXE importa Steam API | API cliente Valve y origen potencial de tickets; no hay ticket ni login generado. | CONFIRMADO PE/versión |
| OpenSSL | libeay32.dll y ssleay32.dll, FileVersion 1.0.1f | Biblioteca crypto/TLS declarada por Meteor/Vivox; versión presente, política TLS de cada flujo no inferida. | CONFIRMADO PE/versión |
| Vivox/oRTP | vivoxsdk_x64, vivoxplatform, vivoxoal, ortp y ortp_x64; imports vx_* del SDK Meteor | Voz/SIP/RTP; hosts de cuenta vienen de respuesta de servicio. | CONFIRMADO interfaces; runtime NO VERIFICADO |
| Twitch/FFmpeg | twitchsdk_64_release, swresample-ttv-0, avutil-ttv-51, libmp3lame-ttv | Streaming/multimedia, catálogo separa destinos de Twitch de infraestructura de juego. | CONFIRMADO artefactos; habilitación NO VERIFICADA |
| cURL/gloox/PlayFab/DeltaDNA | Símbolos/strings y handlers dentro del SDK Meteor, no DLL separada obligatoria | Transporte auxiliar, XMPP y servicios REST/telemetría; detalle en auth y endpoints. | CONFIRMADO referencias; versión exacta por componente donde recuperable |
| PhysX/APEX/CUDA | DLL NVIDIA listadas y sus recursos/imports | Física/ropa y soporte GPU; presencia no atribuye funciones de red. | CONFIRMADO artefactos |
| wxWidgets/Substance/zlib/NVTT/Bink/Vorbis/Ogg | DLL y nombres de producto/export presentes | Herramientas, recursos, compresión y audiovisual; listados por trazabilidad del bundle. | CONFIRMADO artefactos |
| AgentInterface/atc_api/EasyHook | DLL del bundle y PE disponible | Rol en red/autoprotección NO VERIFICADO; no deducido solo del nombre. | CONFIRMADO presencia; función NO VERIFICADA |

## Todos los ejecutables y DLL: identidad y versiones

La versión `—` significa que FileVersionInfo no recuperó una versión declarada; no significa versión cero. El hash completo manda para comparar muestras. Imports/exports completos permanecen en JSON para no truncarlos.

| Ruta relativa al juego | SHA-256 | FileVersion / ProductVersion | Arquitectura / CLI |
|---|---|---|---|
| `Binaries\AutoReporter.exe` | `25407fa1b48e1ef43193f1b17b5175adf038787b71da3048a3600d1cbdb79aec` | 1.0.5312.25853 / 1.0.5312.25853 | x86; managed CLI |
| `Binaries\Redist\UE3Redist.exe` | `fd2decbe13afad90618acaa12d6e6aa4eeb9155d3dbb80c429ceb2d5d00c4a09` | — / — | x86 |
| `Binaries\Win64\AgentInterface.dll` | `3be73b02b10f19e7064bf8be105153cb834b112062a19b1ef17d56f72fb24740` | — / — | x86; managed CLI |
| `Binaries\Win64\APEX_BasicFSPROFILE_x64.dll` | `f4bfbcecd058faa99d6a1bbe5eccaf536ccf14f57067955bd25f66f19a5478f7` | 1.2.3.3 / 1.2.3.3 | x64 |
| `Binaries\Win64\APEX_BasicIOS_LegacyPROFILE_x64.dll` | `093334b2794e5608ff11a4262d96ebca1a0b3d1c0d55e3d82097a607e26d667f` | 1.2.3.3 / 1.2.3.3 | x64 |
| `Binaries\Win64\APEX_BasicIOSPROFILE_x64.dll` | `a00306e3245e57510ebbea9dbd754f2add9f87df6103fea6ab1936908c07a6b0` | 1.2.3.3 / 1.2.3.3 | x64 |
| `Binaries\Win64\APEX_Clothing_LegacyPROFILE_x64.dll` | `3813494f95a2a78488851e1bd89fc7fa8812e75944fd4580cadfa5e262d8dbcd` | 1.2.3.3 / 1.2.3.3 | x64 |
| `Binaries\Win64\APEX_ClothingGPUPROFILE_x64.dll` | `0bbc834ec9b4b7831b40216a8d06798aa2b16c95c3c96af386e15799975bab5e` | 1.2.3.3 / 1.2.3.3 | x64 |
| `Binaries\Win64\APEX_ClothingPROFILE_x64.dll` | `27f2b5e2d3e5e94f2060b67b57387d0754689b6af40b549b2915f4dfbc6e337f` | 1.2.3.3 / 1.2.3.3 | x64 |
| `Binaries\Win64\APEX_Common_LegacyPROFILE_x64.dll` | `711aaaa406ac9dd2e83951d3fede799383181254536e582df75fd78d5cf0f152` | 1.2.3.3 / 1.2.3.3 | x64 |
| `Binaries\Win64\APEX_Destructible_LegacyPROFILE_x64.dll` | `6bad3f56abd68e92ef1a025ea9c64046ee43bb9b10ae911bc0055ab772633e84` | 1.2.3.3 / 1.2.3.3 | x64 |
| `Binaries\Win64\APEX_DestructiblePROFILE_x64.dll` | `7b5f0eb3c5c302ee246a89d475c422b7382f11809d6579d16e00c03ea51e34d6` | 1.2.3.3 / 1.2.3.3 | x64 |
| `Binaries\Win64\APEX_Emitter_LegacyPROFILE_x64.dll` | `21fa11408c4ab139877266815133886ab6f58825ff6335ff7969dbaf4834ea05` | 1.2.3.3 / 1.2.3.3 | x64 |
| `Binaries\Win64\APEX_EmitterPROFILE_x64.dll` | `b07b36485fbbe28e47e592c2e81fa726835a6a887d24a453572f1ea86d611ebc` | 1.2.3.3 / 1.2.3.3 | x64 |
| `Binaries\Win64\APEX_ExampleFSPROFILE_x64.dll` | `7bc839b9ccf9753caa993200c02f8ca941ff5024337ec3dd173259ac4d3cc205` | 1.2.3.3 / 1.2.3.3 | x64 |
| `Binaries\Win64\APEX_FieldSamplerPROFILE_x64.dll` | `a4600f2b95c210f668b806daff73098d64eeda31ce3fa349ccb6c6744aad3fa2` | 1.2.3.3 / 1.2.3.3 | x64 |
| `Binaries\Win64\APEX_Framework_LegacyPROFILE_x64.dll` | `d17dd14c40530dcca4ace6957ce495f251a63b8272ac1fd933aeb95cacf00ad7` | 1.2.3.3 / 1.2.3.3 | x64 |
| `Binaries\Win64\APEX_IOFX_LegacyPROFILE_x64.dll` | `a604c8c913d5a114e8746df02fc71a07414733205682f1a2c90f725cc1191870` | 1.2.3.3 / 1.2.3.3 | x64 |
| `Binaries\Win64\APEX_IOFXPROFILE_x64.dll` | `d7d998be2dcf11842c099cc88218b717cab530989d491b54495faf646cb12270` | 1.2.3.3 / 1.2.3.3 | x64 |
| `Binaries\Win64\APEX_NxFluidIOSPROFILE_x64.dll` | `89a3cdb6f06ffb3b0d4d6089a50f6c7de9d8b832ad01e6f8d5b678f1c6f1b4a4` | 1.2.3.3 / 1.2.3.3 | x64 |
| `Binaries\Win64\APEX_TurbulenceFS_LegacyPROFILE_x64.dll` | `aa097f5a47179f858e4d22b8fe0b5dea9cd77be56237a6084ab1ac9be3f76b95` | 1.2.3.3 / 1.2.3.3 | x64 |
| `Binaries\Win64\APEX_TurbulenceFSPROFILE_x64.dll` | `9d95b7cf60047ac4a1b1b7741e45235e71e0db15ad51b14bf185e3c33a5fefbd` | 1.2.3.3 / 1.2.3.3 | x64 |
| `Binaries\Win64\ApexFrameworkPROFILE_x64.dll` | `600d440468b58e59e00804dd9f5098eb0fa7fc54226f0012d68f7552fdd0ca39` | 1.2.3.3 / 1.2.3.3 | x64 |
| `Binaries\Win64\atc_api.dll` | `fbef4db537dda0bc6a1f23342973ab9270b8021b9a6789aa3621e953a889c3ae` | — / — | x64 |
| `Binaries\Win64\avutil-ttv-51.dll` | `8919065f6af9ab04f651eff447b8640c53da0dd284ae5d551f5929c3f09a0f83` | — / — | x64 |
| `Binaries\Win64\bink2w64.dll` | `2cd7762823e6e55039af31ba1c398867fc865dfbd68c4e87c19f5f7913e91281` | 1.994m / 1.994m | x64 |
| `Binaries\Win64\cudart64_30_9.dll` | `b523e160527e43c3dc6f4a4d478b1ed78d83444d3106672339df3c2724b126b3` | 6,14,11,3000 / 6,14,11,3000 | x64 |
| `Binaries\Win64\cudart64_41_22.dll` | `aa7e9c0183e86c353a7f4256507d9c607cf3d2fc6073152ccff4a2daf33487af` | 6,14,11,4010 / 6,14,11,4010 | x64 |
| `Binaries\Win64\cudart64_42_9.dll` | `60709dcca6760994395f64a2b4e905aedf57f53ee2ad3c0f4312e83b839cc9d9` | 6,14,11,4020 / 6,14,11,4020 | x64 |
| `Binaries\Win64\cudart64_50_35.dll` | `84e4d028a6ff3d1c29c4c9e50d418f1ebf14b4f5402bd00da8303ac3329aedab` | 6,14,11,5000 / 6,14,11,5000 | x64 |
| `Binaries\Win64\dbghelp.dll` | `16ebea6b65d39debcb3990b8fb883120da75cf617a3b1c6612f4bd54b2d47bc8` | 6.8.0004.0 (debuggers(dbg).070515-1751) / 6.8.0004.0 | x86 |
| `Binaries\Win64\EasyHook64.dll` | `444c9ee2312ee198fb9b651edfed51c7c9eef2fe4bc3fb04bf522bfa094d9ae5` | — / — | x64 |
| `Binaries\Win64\FxGraphLayout_x64.dll` | `ba4e728dd2903d0bf9e474ffc3b286f144ff917f4bc8acd113cbad5bacf344ea` | 1, 0, 0, 1 / 1, 0, 0, 1 | x64 |
| `Binaries\Win64\GRBPROFILE_1_1_api2_x64.dll` | `bcb76c176284f72a65fa03674ec57deff57cab8797dbd094da563d3004fc4083` | 1.2.3.3 / 1.2.3.3 | x64 |
| `Binaries\Win64\HawkenGame-Win64-Shipping.exe` | `4a861ecb4ac7add9554f8daf8f98ad2c12b9cb6c1865a83e8be808beb46ea8a2` | 1, 6, 0, 143417 / 1, 6, 0, 143417 | x64 |
| `Binaries\Win64\libapexeditorwidgets_x64_profile.dll` | `020cf28004b84420e40b65c3c53eadc76efdd436dde4501b016c5fbee21cae0c` | — / — | x64 |
| `Binaries\Win64\libeay32.dll` | `79cc4b787cc7bd0c99525d84ba75289141a95b675b764ad6f6afe57dbf8ea91a` | 1.0.1f / 1.0.1f | x64 |
| `Binaries\Win64\libmfxsw64.dll` | `b5eaa1918ffbdcdfaf91a4895dac20b6325a9f53d20f343b7e7f05ff95c9aabd` | 4.13.6.21 / 4.0.0000760.60139 | x64 |
| `Binaries\Win64\libmp3lame-ttv.dll` | `3b94822242b99636448339f7329a20c16213706c49dca2a8b2241431bba8d19f` | — / — | x64 |
| `Binaries\Win64\libogg_64.dll` | `415c5fbd9c9ee8b736558c0259d6c026c73b6d251c44ec99012e2ff11f64bb70` | — / — | x64 |
| `Binaries\Win64\libresample_x64.dll` | `79ae0566dc63ad27358dddb4d40fe1418beb87f5fc2a65b5f081f003597190e3` | 0, 1, 3, 0 / 0, 1, 3, 0 | x64 |
| `Binaries\Win64\libsndfile_x64-1.dll` | `b49db58121da0dad1132c5cb00cd76c00ee7368a3ce4203dc8366f1ef4b50daa` | 4, 6, 0010, 20472 / 4, 6, 0010, 20472 | x64 |
| `Binaries\Win64\libvorbis_64.dll` | `657b4fee44131b6607bed8c38210a38e973993324c75263c1bc31a85f6156bd9` | — / — | x64 |
| `Binaries\Win64\libvorbisfile_64.dll` | `d52a01dab7d7f6a178ccffa61a7cac6545bbe452a6037ded1ae9fe7bbd88745f` | — / — | x64 |
| `Binaries\Win64\LightFX.dll` | `4a52d8e4db0a6eebb12d14723a38fc8899e7e4a3e97c2557f049fcf825791fcc` | 1.0.7.0 / 1.0.7.0 | x64 |
| `Binaries\Win64\Meteor.Sdk.Cpp.Win32.Client64.dll` | `f63fef02ffb517ca7844a769d03412ad82bd22198397302ee3c090c63a970c57` | — / — | x64 |
| `Binaries\Win64\nvtt_64.dll` | `b6a9c27741bad4f01b4dbe763d9c033ee717a94f5e438d4baa125fd2c29f31cf` | 2, 0, 6, 0 / 2, 0, 6, 0 | x64 |
| `Binaries\Win64\NxCharacter64.dll` | `4b90704de237d4ceacc880e1de74b5e3c15f4982100dfa42183c51106d2a6e84` | — / — | x64 |
| `Binaries\Win64\ortp_x64.dll` | `9023efdd1b4a9cf025d0031961d903636fea9a29b9c2fb16375996378fb61093` | — / — | x64 |
| `Binaries\Win64\ortp.dll` | `d1dc517e745759f31e7c55e5573697ae71faa3f0853aee77f50dba3154e70a79` | — / — | x64 |
| `Binaries\Win64\p4dn.dll` | `6046d6505a7d66157e704474d2b29092fb18b159760a51ee60e12445b5391bc5` | — / — | x64; managed CLI |
| `Binaries\Win64\PhysX3GpuPROFILE_x64.dll` | `cb4d5c772fe6b23874cf8f77d1771238ff7406969aeb5c9678aef62456e99f46` | — / — | x64 |
| `Binaries\Win64\PhysXCooking64.dll` | `5a57cb8e11b78e73a75f5c19923df3b7bf947f8151fd681d283199168dcd8e01` | — / — | x64 |
| `Binaries\Win64\PhysXCore64.dll` | `6d69cb61bdc564f6b0277af9fb9af5b71eaec09116142d5c926b81e7ad33df93` | — / — | x64 |
| `Binaries\Win64\PhysXDevice64.dll` | `02ffb0d84fa593f3f4919bf87009750f8821f03860db58b3031ac312597aba10` | 12, 5, 9, 1 / 12, 5, 9, 1 | x64 |
| `Binaries\Win64\PhysXExtensions64.dll` | `aab48d0b04b1e49da281a0bdddf044d89a0e9437e6bc258739f1d40f755af4a0` | 2, 8, 4, 1 / 2, 8, 4, 1 | x64 |
| `Binaries\Win64\PhysXLoader64.dll` | `6d4a6fbcad56cc424e9a1dfab695d5f891288b51cfe6d940334935c4db939b80` | — / — | x64 |
| `Binaries\Win64\ssleay32.dll` | `6b19070c84e92642fd8daae373309af8b7a3de49de7fc2374ebf5744feb631c9` | 1.0.1f / 1.0.1f | x64 |
| `Binaries\Win64\StandaloneSymbolParser.dll` | `b34fe4bcfdc58ef9aac581a991683c92410c4dc8c01b0355c5152b65274b9639` | — / — | x64; managed CLI |
| `Binaries\Win64\steam_api64.dll` | `38f6a1dd1e738e142160322638dae7b88dabcc30eea3f15c58eb08c5341f9b29` | 03.75.32.07 / 01.00.00.01 | x64 |
| `Binaries\Win64\substance_linker.dll` | `ed8065a55a0b9671da6451c12edb86b94e40f8ab6468825b1c440b53d5112cf0` | — / — | x64 |
| `Binaries\Win64\substance_sse2_blend.dll` | `00b59dc9bfe89d847365eb95b6782b96d093dce4c01ebfdcb7b3d57bca5850e8` | — / — | x64 |
| `Binaries\Win64\swresample-ttv-0.dll` | `27da723630312fc6f4ce09ab4d1af2722d8db59b98df4d387060a725f154fc1b` | — / — | x64 |
| `Binaries\Win64\tbbmalloc.dll` | `32b5676ccb79a00b68102d55d3d672aaa391c4e0992519783903b65355ed41cc` | 3, 0, 0, 0 / 3, 0, 0, 0 | x64 |
| `Binaries\Win64\twitchsdk_64_release.dll` | `1ee603670cc5c4a1f946ba82f066c6b7bced3c69729536425a97176156a65b76` | — / — | x64 |
| `Binaries\Win64\vivoxoal_x64.dll` | `9135f28c0d37cb688b54afe1698e13edd6872a10712be993d90cab0b2b7684e7` | 4, 6, 0010, 20472 / 4, 6, 0010, 20472 | x64 |
| `Binaries\Win64\vivoxplatform.dll` | `358fa8a00a8a704b2f6d0731884d1bbfa87a47e518f52378f2ceed9fdc2050e4` | 4, 7, 0003, 22888 / 4, 7, 0003, 22888 | x64 |
| `Binaries\Win64\vivoxsdk_x64.dll` | `a0140b99a85083308e9bf337baf7cd78f1a3e975fb4e94869d172a3cf960d9a0` | 4, 7, 0003, 22888 / 4, 7, 0003, 22888 | x64 |
| `Binaries\Win64\WindowsTools_x64.dll` | `44ce62dbc8b49a069a908a366dc26bd89449fce0149d36bd78253f3796e9d43f` | — / — | x64; managed CLI |
| `Binaries\Win64\wxmsw28u_adv_vc_custom_64.dll` | `a18a43abc4364e56d7ff94fdcd317a8d9ab1dda18423287a4ddf20fec6658ad2` | wxWidgets Library 2.8.9 / 2.8.9 | x64 |
| `Binaries\Win64\wxmsw28u_aui_vc_custom_64.dll` | `e7826b2d72f8d3cf5a25fdbcc62992069d3afafaec94e028b8e7e3fa0cef4503` | wxWidgets Library 2.8.9 / 2.8.9 | x64 |
| `Binaries\Win64\wxmsw28u_core_vc_custom_64.dll` | `98842d3cae62612fcf45a674f99b3c6f25d98cdbc82371c8928ae9e6a30e1c90` | wxWidgets Library 2.8.9 / 2.8.9 | x64 |
| `Binaries\Win64\wxmsw28u_html_vc_custom_64.dll` | `023211396926c6492e10cda2dafb54f74beefdcd538508775353da9a9babdd4f` | wxWidgets Library 2.8.9 / 2.8.9 | x64 |
| `Binaries\Win64\wxmsw28u_richtext_vc_custom_64.dll` | `9d7b7813645556078344f2e7006649caae2dcb5e458b4f8c05f816fb2ef0d864` | wxWidgets Library 2.8.9 / 2.8.9 | x64 |
| `Binaries\Win64\wxmsw28u_vc_custom_64.dll` | `8aecdb235fbfd27a304bf9c9fdad973c3fa74074f6e8c22073f0478767673e55` | wxWidgets Library 2.8.9 / 2.8.9 | x64 |
| `Binaries\Win64\wxmsw28u_xml_vc_custom_64.dll` | `24ae596c5187958c69d66c0d22fc36526184831b1e30a9bc6b25ee48e50f1630` | wxWidgets Library 2.8.9 / 2.8.9 | x64 |
| `Binaries\Win64\wxmsw28u_xrc_vc_custom_64.dll` | `306ae65da0a54da687ee2af1980cc8f77743ea46bba0ae37134e879fd35532b5` | wxWidgets Library 2.8.9 / 2.8.9 | x64 |
| `Binaries\Win64\wxmsw28ud_adv_vc_custom_64.dll` | `d9afe84b9a725c32e5371436f37d784fca70b73310f5ef7fd9eed729111c8839` | wxWidgets Library 2.8.9 / 2.8.9 | x64 |
| `Binaries\Win64\wxmsw28ud_aui_vc_custom_64.dll` | `b3e515c71f8d906d20c653b735a77aabfba0fdc593c962330f6ce9e2b3132743` | wxWidgets Library 2.8.9 / 2.8.9 | x64 |
| `Binaries\Win64\wxmsw28ud_core_vc_custom_64.dll` | `ebae33b408e3031a36514b54b43679290f80e3f16bdf72ded6edbadaa5e705b0` | wxWidgets Library 2.8.9 / 2.8.9 | x64 |
| `Binaries\Win64\wxmsw28ud_html_vc_custom_64.dll` | `df20d7d915e6437cef63d125cc1d2e7a3a6dad8521a5c8f8ff06605f7658ff19` | wxWidgets Library 2.8.9 / 2.8.9 | x64 |
| `Binaries\Win64\wxmsw28ud_richtext_vc_custom_64.dll` | `15403114c5c0eee5ccb83eae2830bb61e9cfb0693483aa449a4c88ee0f12d22f` | wxWidgets Library 2.8.9 / 2.8.9 | x64 |
| `Binaries\Win64\wxmsw28ud_vc_custom_64.dll` | `abb21561ac8d4413512f04a27b18551616f5f64e5a5823b06daefd413961bcc0` | wxWidgets Library 2.8.9 / 2.8.9 | x64 |
| `Binaries\Win64\wxmsw28ud_xml_vc_custom_64.dll` | `b3c2bb9d0dd9c56db19fda766671be2c516114ec131dcecbf0669e90e2c03239` | wxWidgets Library 2.8.9 / 2.8.9 | x64 |
| `Binaries\Win64\wxmsw28ud_xrc_vc_custom_64.dll` | `c4cab350a9b11ef3d0c00ade521f4efd6b2ef96a490098e0ebe42a1c5daf7a12` | wxWidgets Library 2.8.9 / 2.8.9 | x64 |
| `Binaries\Win64\zlib1.dll` | `e9fa5c36098de0a09b54d0567a48b53d9022aa95c3b4e7cad1e5d2ad1a993988` | 1.2.3 / 1.2.3 | x86 |

## Configuración y paquetes de código relevantes

| Ruta relativa | Tamaño | SHA-256 |
|---|---:|---|
| `Binaries\Win64\steam_appid.txt` | 6 | `7f8af64056cc012f81841069a6bb93ad471e5f738a82e05a4981030a6da34233` |
| `Binaries\Win64\StormConsumerSettings.ini` | 133 | `08eda2cb20539996b7c1a91480c1ee2f64e02743c9bbee0bfc4f3543d8fec3d9` |
| `Engine\Config\BaseEngine.ini` | 32607 | `b550c921f85f41aa947e4502990e6f381163e82836ef64141aaff05cb5f222d9` |
| `Engine\Config\BaseGame.ini` | 4038 | `76e40c60fbe500654c699e385394f3da6be08cd2434500a37b4a1c02be27a485` |
| `HawkenGame\Config\DefaultEngine.ini` | 18585 | `6f839d73d16f512fbbf29e4d35ea12ba1e8604c8372ea72cad90dabff2526599` |
| `HawkenGame\Config\DefaultGame.ini` | 15056 | `dc2a794217d13d8e7af4719c087dfbbfb4e36cbe433dd8474caaa893ddd7be00` |
| `HawkenGame\Config\DefaultOnlineConfig.ini` | 67 | `34e25f2e24b015cb47d6a01386086f9b429463489e802a00e9d9c3332f63a847` |
| `HawkenGame\CookedPCConsole\Core.upk` | 39092 | `c6351b52aae3a78f0819963518be8bfcac93864fe73b8cdd5d07990662474375` |
| `HawkenGame\CookedPCConsole\Engine.upk` | 1097168 | `8fff59b404ea4f9dd41330da618432fe50e4a26ce7487379efeb76ce0c4e859c` |
| `HawkenGame\CookedPCConsole\HawkenGame.upk` | 101014208 | `d1a33a064de4451e5a39e13a28e4a8f2913ce6d9b54ca65bae47118d32849ed0` |
| `HawkenGame\CookedPCConsole\IpDrv.upk` | 119552 | `432087362da101de1189e88a260e4a18c016320b8b9ccd9911df59eebe04b4ef` |
| `HawkenGame\CookedPCConsole\OnlineSubsystemMeteor.upk` | 99223 | `bab9ad82bd5fae5afe30707513caa27605c7ece8f296461678ae07cd759ed310` |
| `HawkenGame\CookedPCConsole\Startup.upk` | 19311852 | `53120817c845db95273b7d0761830e85241bf8b49d8f7bbe0956df4d7c0963a4` |

## Herramientas y evidencia REA/Ghidra

Skill REA v34 leída íntegramente; guías native/artifacts y evidence/workflows consultadas. REA 6.3.0 con 139 herramientas verificado mediante una sesión stdio local, sin registrar un nuevo MCP global. [rea-artifact-main.json](evidence/rea-artifact-main.json), `ev_27a70b00609b60b18ceca80c9918417751cb7c28befa2575a45dbed3cc2ad637`, identifica el PE original; [rea-autoreporter-managed.json](evidence/rea-autoreporter-managed.json), `ev_36b3bf3514c82e02213fd037cc4405675c859246baf454511818dd12d04c7e6d`, identifica CIL sin ejecutar CLR.

Ghidra 12.1.4 se encontró instalado. Tras autorización del usuario se añadió JDK Temurin 21.0.12.1 portátil dentro de `tools`, con SHA-256 del ZIP cotejado contra metadatos oficiales. [portable-jdk.json](evidence/portable-jdk.json), [jdk-download-metadata.json](evidence/jdk-download-metadata.json) y [rea-ghidra-provider-doctor.json](evidence/rea-ghidra-provider-doctor.json) conservan rutas, checksum y provider disponible. Las variables solo se aplicaron a procesos de análisis; la configuración global no se cambió. El doctor con scope proveedor es healthy; las integraciones MCP globales siguen sin registro.

La publicación npm usada rechaza DLL Windows (`target_role_unsupported`) en [rea-sdk/open-binary.json](evidence/rea-sdk/open-binary.json). Se usa Ghidra headless directo para el SDK en un proyecto privado de esta investigación; no se modifica el guard de REA ni la cabecera DLL. Las conclusiones nativas deben citar las decompilaciones efectivas, nunca solo el doctor.

Los cinco lotes iniciales del SDK en el proyecto privado persistente HawkenSDK son: [funciones](evidence/ghidra-sdk-functions.jsonl), [servicios](evidence/ghidra-sdk-services.jsonl), [auxiliares](evidence/ghidra-sdk-auxiliary.jsonl), [schemas](evidence/ghidra-sdk-schemas.jsonl) y [resultados](evidence/ghidra-sdk-results.jsonl). Son 55 funciones solicitadas con pseudocódigo, referencias, identidad/hash y estado de decompilación. Se añadieron [cuentas/eventos](evidence/ghidra-sdk-account-events.jsonl), [contratos](evidence/ghidra-sdk-account-contracts.jsonl), [complementos](evidence/ghidra-sdk-account-final.jsonl), [metadatos](evidence/ghidra-sdk-account-metadata.jsonl) y [parsers](evidence/ghidra-sdk-account-parsers.jsonl). [Los dos modelos finales](evidence/ghidra-sdk-final-models.jsonl) cierran los parsers GameItem y Transaction. Antes de la ampliación UPK, el conjunto contenía **98 registros / 98 entradas únicas SDK**. Los lotes posteriores reutilizan el proyecto con noanalysis/readOnly. No hay ejecución de instrucciones del juego.

El EXE completó overview y un batch de 29/29 funciones, sin fallos, por REA/Ghidra. [binary-overview.json](evidence/rea-main-run2/binary-overview.json), Evidence ID `ev_732ca8087b47fc6de7ef402ffa3dc06845d44dd7265328dbd04fde2eb07fbadf`, informa 9 segmentos, 133.184 procedimientos reconocidos y 45.047 strings; son resultados del analizador, no equivalencia con la cantidad de funciones originales del código fuente. [network-functions.json](evidence/rea-main-run2/network-functions.json), `ev_f40e06077c59faa017ce58b417dd5336de0da3db2a4af0895c3744c66a44651c`, retiene las 29 decompilaciones; [ghidra-main-functions.jsonl](evidence/ghidra-main-functions.jsonl) es una normalización trazable de ese batch. Se exportó [el bundle completo](evidence/rea-main-complete-bundle.json) y se cerró la sesión. El primer intento con presupuesto 330 s falló; el siguiente permitió 900 s y terminó. Ningún timeout previo se cuenta como evidencia exitosa.

Se guardó también un proyecto persistente HawkenMain para seguir callers, factories, sesiones, crypto y recepción. Antes de la ampliación UPK se habían completado **82 registros EXE / 80 entradas únicas**, contando el batch inicial; dos constructores aparecen en más de un lote. [flow](evidence/ghidra-main-flow.jsonl), [socket/auth](evidence/ghidra-main-auth-socket.jsonl), [gameplay](evidence/ghidra-main-gameplay.jsonl), [sesión](evidence/ghidra-main-session-consumers.jsonl), [wrapper](evidence/ghidra-main-session-packet.jsonl), [primitivas](evidence/ghidra-main-crypto-primitives.jsonl), [wire](evidence/ghidra-main-crypto-wire.jsonl) y [recepción](evidence/ghidra-main-packet-receiver.jsonl) conservan pseudocódigo y referencias. [Lector de paquete](evidence/ghidra-main-final-reader.jsonl), [helpers de bits](evidence/ghidra-main-final-bits.jsonl), [constructor](evidence/ghidra-main-bitreader-ctor.jsonl) y [métodos concretos](evidence/ghidra-main-bitreader-methods.jsonl) siguen ACK/bunch y límites. [Copia y entrega de bunch](evidence/ghidra-main-bunch-delivery.jsonl) recuperan el reambalaje de bits y cola ordenada/dispatch; los consumers finales de canal siguen parciales. La consulta inicial de AtEnd RVA `0x6d7d0` no encontró función reconocida; ese probe se conserva en el lote de métodos. Se definió la función sólo en memoria dentro de un proceso readOnly y se recuperó en [el lote AtEnd](evidence/ghidra-main-bitreader-atend.jsonl), con [instrucciones](evidence/bitreader-atend-instructions.json). No se cuenta el probe como decompilación exitosa ni se cambió el PE o se guardó esa definición en el proyecto. El autoanálisis de la importación persistente EXE alcanzó el límite de 180 s; la extracción pedida terminó y guardó el proyecto. No se atribuye cobertura completa de xrefs/funciones a ese autoanálisis limitado ni se interpreta una ausencia de xref como ausencia de uso. Los lotes siguientes se procesaron con noanalysis/readOnly; no reejecutan el juego.

[remaining-network-scan.json](evidence/remaining-network-scan.json) complementa los barridos principales: 74 PE restantes, configuraciones/textos y 366 cabeceras empaquetadas; 1.028 coincidencias léxicas, sin errores. Incluye falsos positivos y destinos de herramientas; no equivale a 1.028 endpoints operativos. Las cabeceras no sustituyen al parsing de los seis paquetes de código ni a la inspección de cada recurso comprimido.

Documentación primaria consultada: [REA Windows Ghidra](https://github.com/morluto/rea/blob/main/docs/windows-ghidra-p0.md), [Ghidra 12.1.4 Getting Started](https://github.com/NationalSecurityAgency/ghidra/blob/Ghidra_12.1.4_build/GhidraDocs/GettingStarted.md) y [verificación de archivos Temurin](https://adoptium.net/installation/archives/). El soporte descrito en main no reemplaza la capacidad comprobada de la publicación instalada.

## Descompilación de los paquetes UPK

**INV-003 — CONFIRMADO (herramienta y derivados).** Se obtuvo [UE Explorer 1.6.2](https://github.com/UE-Explorer/UE-Explorer/releases/tag/1.6.2) de su publicación oficial y se comprobó el SHA-256 del archivo portátil, `085267f3192acb27c2a6a98367b2e659a202a80adda79d8562cddd3e7e8e53d9`, contra el digest de GitHub. Su `Eliot.UELib.dll` 1.13.0 tiene SHA-256 `64ebfa8b290429b9d5b5e81f5018734c03ded27bb814c69b904cc269268f7bbc`. La herramienta permanece en `tools/ue-explorer-1.6.2/ue-explorer`, fuera del juego; .NET Framework 4.8 ya estaba instalado. [Manifiesto de instalación](evidence/ue-explorer-1.6.2-install.json), [investigación de herramientas](evidence/upk-tool-research.json). Se empleó un lector de análisis local enlazado con esa biblioteca, sin ejecutar el cliente ni la interfaz con actualizador de UE Explorer.

El tag personalizado y la versión 892/licensee9 de estos UPK exigieron seleccionar explícitamente el lector Hawken y validar su estructura. [El diagnóstico de cabecera](evidence/upk-header-research.json) conserva el código fuente de la biblioteca utilizado. [normalize_package_copies.py](analysis/upk/normalize_package_copies.py) reconstruye sólo seis copias de análisis: retira los descriptores de compresión de la cabecera derivada y recoloca la cola original del resumen. Los payloads y offsets virtuales de tablas/objetos permanecen iguales. [upk-normalized-copies.json](evidence/upk-normalized-copies.json) registra hashes originales, virtuales y normalizados, rangos de cabecera y hash del payload. Esta transformación no se escribió a ningún UPK instalado.

Los seis procesos Core, Engine, IpDrv, OnlineSubsystemMeteor, HawkenGame y Startup terminaron con exit0, cero fallos en sus manifests y **21.977 archivos de clases/funciones solicitados emitidos**. [Resumen y advertencias](evidence/upk/decompilation-summary.json), [salidas y manifests](evidence/upk/packages), [fuente del lector](analysis/upk/ExportUnrealScripts.cs). El recuento incluye imports; no se interpreta como 21.977 cuerpos originales recuperados.

| Paquete | Salidas emitidas | Hash del original |
|---|---:|---|
| Core | 352 | Tabla de paquetes anterior |
| Engine | 7.004 | Tabla de paquetes anterior |
| IpDrv | 943 | Tabla de paquetes anterior |
| OnlineSubsystemMeteor | 1.057 | Tabla de paquetes anterior |
| HawkenGame | 12.604 | Tabla de paquetes anterior |
| Startup | 17 | Tabla de paquetes anterior |

[La validación independiente de alcance](evidence/upk/decompilation-scope-validation.json) separa **19.938 records de exports positivos propios** y **2.039 imports**. Hay **17.335 UFunctions propias**: 3.175 con flag native y 14.160 no nativas con ScriptSize positivo. Los 1.362 imports de UFunction tienen ScriptSize0 y no acreditan sus cuerpos: por ejemplo, `Engine.PlayerController.AllowTextMessage` importada por HawkenGame se debe consultar en su export positivo de Engine. Los 21.977 hashes de `.uc`, seis originales y seis entradas normalizadas coinciden con sus manifests. Se resuelven objetos por fullpath y sufijo FName; ordinal/NetIndex del lector no se equiparan al índice del export original.

**Límite CONFIRMADO del resultado; corrección completa de source/runtime NO VERIFICADA:** todos los records conservan `OUTPUT_REQUIRES_REVIEW`. Se observaron 121.725 placeholders `__NFUN_n__` de 411 IDs en los outputs; [el mapa de tokens Core](evidence/game/upk-native-token-map.json) recupera 157 firmas por evidencia de este mismo paquete. Engine y HawkenGame registraron respectivamente 372 y 534 advertencias PropertyTag, por lo que sus defaults no se consideran íntegramente recuperados. Las declaraciones native necesitan sus implementaciones EXE/DLL: los nuevos lotes [bindings](evidence/ghidra-main-upk-native-bindings.jsonl), [helpers](evidence/ghidra-main-upk-auth-helpers.jsonl), [consumers](evidence/ghidra-main-upk-native-consumers.jsonl), [implementaciones virtuales](evidence/ghidra-main-upk-virtual-implementations.jsonl), [origen del override](evidence/ghidra-main-upk-override-source.jsonl), [productores de listing](evidence/ghidra-main-upk-listing-producers.jsonl) y [eventos/override](evidence/ghidra-main-upk-event-and-override.jsonl) reutilizan Ghidra noanalysis/readOnly y citan el SHA original EXE. Los informes de protocolos identifican las funciones concretamente revisadas.

## Cobertura final de los 360 UPK y del código adicional

**INV-004 — CONFIRMADO (bytes, tablas e identidad de extracción).** [all-package-endpoint-scan.json](evidence/upk/all-package-endpoint-scan.json) cubre **360/360 originales**, **0 errores de lectura**, **18.804 bloques zlib** y **3.837 strings candidatas** en 123 paquetes. Cada entrada conserva ruta/SHA-256 original, posiciones virtuales, propietario/export cuando existe y mapa hacia bloques comprimidos. Son candidatos léxicos, no 3.837 endpoints activos. El catálogo distingue metadata, shader, audio, recursos web, ejemplos stock y funciones Hawken; valores recibidos o construidos sólo en ejecución siguen NO VERIFICADOS.

Las tablas contienen **22.519 exports propios Class/Function: 3.160 clases y 19.359 funciones**, repartidos en **75 paquetes con código propio**. La extracción procesó **76 paquetes**, porque también incluyó Startup, cuyos 17 outputs iniciales son imports. [additional-code-package-scope.json](evidence/upk/additional-code-package-scope.json) identifica los 70 paquetes adicionales a la selección inicial y sus 2.581 exports. La cobertura aceptada se divide así:

| Conjunto | Exports Class/Function propios | Salidas seleccionadas | Evidencia de identidad |
|---|---:|---:|---|
| Seis paquetes iniciales, incluido Startup | 19.938 | 21.977, incluidos 2.039 imports | [Audit independiente](evidence/upk/decompilation-scope-validation.json), original/entrada/output SHA y resolución de imports. |
| BaseAI, GameFramework, GFxUI, UDKBase | 1.994 | 1.994 | [Primer resumen adicional](evidence/upk/additional-decompilation-summary.json), paths/outputs cotejados; se reutilizó esta selección válida. |
| Otros 66 paquetes cooked | 587 | 587 | [Resumen indexado final](evidence/upk/indexed-additional-decompilation-summary.json): 587/587 triples índice-export/offset-virtual/tamaño, 587 hashes de output y 66 hashes fuente/copia coincidentes. |
| Total aceptado | 22.519 | 24.558 | Representación de exports/declaraciones, sin equiparar native con cuerpo C++ ni imports con funciones propias. |

El primer filtro basado en basename dejó 66 paquetes sin outputs: sus ForcedExport usan el namespace `HawkenGameContent`. Una pasada intermedia empleó el lector anterior y no aportó los campos de identidad requeridos; [su resumen permanece invalidado](evidence/upk/owned-additional-decompilation-summary.json). No se borraron manifests/logs. Se compiló [ExportOwnedUnrealScripts.cs](analysis/upk/ExportOwnedUnrealScripts.cs), selector de ExportTable positivo con índice/offset/tamaño; `HawkenOwnedUpkReader.exe` tiene SHA-256 `09c02c4670675de70617e40b5c604c21c29bf0a9166f19205011691b2655cdb7`. Tras un probe positivo, 65 procesos nuevos terminaron exit0 y se reutilizó AI_COMMON_SF. [cooked-path-corrections.json](evidence/upk/cooked-path-corrections.json) conserva los 587 mappings, el path sintético del scan y el GetPath real; no se alteró el scan histórico ni los originales.

**Semántica íntegra NO VERIFICADA:** los records siguen `OUTPUT_REQUIRES_REVIEW`. La pasada final conserva **668 líneas stderr en 63 paquetes**: 394 diagnostics PropertyTag, 214 trailing object data, 10 parse warnings ImportExportGuidsTable y sus líneas de stack/error. Los logs completos están enlazados por paquete. Exit0 y coincidencia de hashes no prueban defaults completos, bytecode correcto en toda función, interpretación integral de SWF/assets ni compatibilidad del servidor.

La revisión funcional adicional está en [upk-additional-reviewed-functions.json](evidence/game/upk-additional-reviewed-functions.json): 28 exports (22 funciones/6 clases) con hashes y coordenadas, además de la selección inicial de 87 funciones de juego y los consumidores auth/web clasificados. Los cuerpos revisados distinguen modos/defaults, tutorial, mensajes y turrets; un nombre como SendOutDamageXPOnDeath no basta para afirmar que concede XP.

La ampliación nativa reutilizó los proyectos Ghidra privados: [productores HTTP](evidence/ghidra-main-upk-http-producers.jsonl), [región/conversión de eventos](evidence/ghidra-main-upk-region-event-final.jsonl), [callback de query](evidence/ghidra-main-upk-query-callback.jsonl), [consumers SDK](evidence/ghidra-sdk-upk-linked-consumers.jsonl), [cola de eventos](evidence/ghidra-sdk-upk-event-transport.jsonl) y [worker](evidence/ghidra-sdk-upk-event-worker.jsonl). El corte previo fue **139 decompilaciones EXE / 135 entradas únicas** y **104 SDK / 104 entradas únicas**, cotejado en [validación histórica](evidence/final-research-validation.json). La revisión Creator añade 17 entradas exitosas: [wrappers/serializer](evidence/ghidra-main-creator-functions.jsonl), [créditos](evidence/ghidra-main-creator-credit.jsonl), [constructores](evidence/ghidra-main-creator-wallet-task.jsonl) y [métodos de tareas](evidence/ghidra-main-creator-wallet-method.jsonl). El conjunto central actual queda en **156 decompilaciones EXE / 152 entradas únicas**, **104 SDK / 104 únicas**, **256 funciones únicas en total**; las direcciones se normalizan numéricamente. Identidades/completitud del corte actual en [validación Creator](evidence/creator-revision-validation.json). El probe histórico sin función no cuenta como decompilación exitosa. Ninguna extracción ejecutó instrucciones del juego.

La revisión también cotejó imports normales/diferidos de DINPUT8/DXGI y la ausencia acotada de VERSION en 86 PE, con bytes del EXE original, sin instalar DLL alguna: [factibilidad de puente](evidence/dll-bridge-feasibility.json). Por la decisión de aplazar UPK nuevos se rehasharon **360/360 paquetes originales**, todos coincidentes, sin añadidos/ausentes/alterados: [manifiesto](evidence/original-upk-sha256-manifest.json). El manifiesto es evidencia local sin firma Valve ni enforcement remoto implementado; no reemplaza GUID ni demuestra bytes cargados por un cliente ajeno.

## Límite de cobertura

Inventario/hash completo no equivale a recuperación exhaustiva de todo el protocolo. Se conservan todas las identidades y el linkage PE; análisis de funciones, paquetes, flujos y datos dinámicos está delimitado en cada informe y en [HAWKEN_UNKNOWN_AND_BLOCKERS.md](HAWKEN_UNKNOWN_AND_BLOCKERS.md). No hubo observación de tráfico, resolución DNS, conexión a servicios de Hawken ni captura de credenciales.


## Mapa global reutilizable — 2026-10-13

Este corte consolida la instalación completa y las investigaciones anteriores. No reinicia el análisis de los binarios. Se conservaron los proyectos Ghidra `HawkenMain`/`HawkenSDK` y todos los dossiers, incluidos los que proceden de sesiones temporales. El export nuevo sólo lee metadata de esas bases: `readOnly/noanalysis`, sin descompilador, sin crear funciones ni ejecutar el juego.

| Capa | Cobertura verificada | Qué permite consultar |
|---|---:|---|
| Identidad del árbol original | 946/946 SHA-256 y tamaños coincidentes | Archivo y versión exactos; cero originales ausentes o alterados. |
| Interfaces PE | 3 EXE + 83 DLL; 399 descriptores normales/diferidos | Módulos importados, funciones/ordinales/IAT, exports y arquitectura. |
| Paquetes | 360 UPK; 75 con exports de código propios seleccionados | Names/Imports/Exports, clases, funciones, offsets virtuales y hashes. |
| Correspondencia de código UPK | 22519 exports propios; 24558 salidas UC canónicas | El mapa distingue imports, declaraciones native y código que requiere revisión. |
| Base Ghidra del cliente | 131954 funciones conocidas; 241766 relaciones de llamada | Entry, nombre, firma propuesta, rangos de cuerpo y callees conocidos. |
| Base Ghidra de Meteor | 14371 funciones conocidas; 28262 relaciones de llamada | El mismo índice sobre la DLL de servicios. |
| Dossiers con pseudocódigo retenido | 886 identidades SHA+entry: EXE611, Meteor246, Steam29 | Reutilizar el cuerpo ya recuperado, sus fuentes y sus variantes. No significa 886 funciones íntegramente comprendidas. |

**Evidencia:** [validación de los originales](../../implementation/evidence/complete-artifact-map-20261013/validation/artifact-map-validation.json), [matriz de los PE](../../implementation/evidence/complete-artifact-map-20261013/pe/coverage-matrix.json), [mapa UPK](../../implementation/evidence/complete-artifact-map-20261013/upk/inventory-and-coverage.json), [resumen de bases Ghidra](../../implementation/evidence/complete-artifact-map-20261013/ghidra/summary.json) y [índice nativo consolidado](../../implementation/evidence/complete-artifact-map-20261013/parent/map-summary.json). Las interpretaciones funcionales permanecen en los informes de protocolos enlazados abajo.

### Consulta antes de una nueva investigación

1. Identificar el archivo por ruta+SHA; consultar su fila PE o paquete. Las coincidencias de basename de DLL son candidatos del bundle, no prueba de resolución efectiva por Windows.
2. Para una función nativa consultar [dossiers únicos](../../implementation/evidence/complete-artifact-map-20261013/parent/native-functions.json) y [ocurrencias con archivo/hash/línea](../../implementation/evidence/complete-artifact-map-20261013/parent/native-occurrences.csv). Cada RVA se calcula usando la base del programa identificado. Se conservan las variantes de pseudocódigo; no se sobrescribe una con otra ni se suman copias como funciones nuevas.
3. Si falta el dossier, consultar el índice completo de la base [cliente](../../implementation/evidence/complete-artifact-map-20261013/ghidra/main-functions.jsonl) o [Meteor](../../implementation/evidence/complete-artifact-map-20261013/ghidra/sdk-functions.jsonl), y sus callees. Hay 38 dossiers del EXE cuyo entry no existe en la base principal: están preservados en el índice consolidado, no se consideran perdidos ni se vuelven a extraer sólo por esa ausencia.
4. Para Unreal consultar [paquete→clase→función](../../implementation/evidence/complete-artifact-map-20261013/upk/package-class-function-map.jsonl) y [bindings positivos UPK→EXE](../../implementation/evidence/complete-artifact-map-20261013/upk/native-binding-examples.json). Export físico, ordinal de salida UC, NetIndex, RVA y offset virtual no son intercambiables.
5. Abrir el hallazgo en su informe. Sólo una pregunta concreta sin respuesta, un input cambiado o una salida previa incompleta justifican nueva extracción. Mantener el archivo/hash, coordenadas, observación, interpretación y estado CONFIRMADO/PROBABLE/NO VERIFICADO.

El [generador del índice](analysis/build_research_map.py) procesa los JSONL retenidos y las interfaces PE, sin cargar DLL ni tocar el juego. El [export de metadata Ghidra](analysis/ExportKnownFunctionMap.java) recorre únicamente las funciones existentes. Los índices de [dependencias PE](../../implementation/evidence/complete-artifact-map-20261013/parent/pe-module-edges.csv), [llamadas recuperadas](../../implementation/evidence/complete-artifact-map-20261013/parent/native-call-edges.json) y [secciones de los siete informes](../../implementation/evidence/complete-artifact-map-20261013/parent/report-sections.csv) permiten localizar las fuentes de cada relación.

### Alcance y brechas

**CONFIRMADO:** identidad e interfaces de los originales, correspondencia de tablas/salidas UPK indicada y metadata/dossiers de este corte. **NO VERIFICADO:** recuperación semántica de todo el código, todas las funciones posibles del PE, todos los despachos virtuales y funcionamiento completo de una partida. El autoanálisis histórico del EXE terminó por timeout; exportar toda la base no vuelve exhaustivas sus referencias negativas. Los errores/avisos del lector UPK y sus `OUTPUT_REQUIRES_REVIEW` siguen vigentes. Los 1170 registros de pseudocódigo son ocurrencias retenidas, incluidas copias; no equivalen a 1170 trabajos de Ghidra. Cinco líneas de un antiguo archivo llamado `diagnostics.jsonl` son texto de diagnóstico, no dossiers; se conservan separadas. Cero errores al interpretar los dossiers aceptados.

Se detectaron tres archivos añadidos desde el inventario original: `HawkenGame/PropSheetCache.txt`, `Binaries/Win64/hawken-client.toml` y `Binaries/Win64/dinput8.dll`. La validación los identifica por separado; no forman parte de los 946 originales. El hash del proxy instalado difiere del build de puente identificado, de modo que su correspondencia exacta queda pendiente. No se reemplazó ni eliminó ninguno.

La skill REA34 y sus guías se reutilizan. El diagnóstico actual de la CLI REA6.3.0 detecta registro MCP Codex ausente/desalineado; no hay herramientas REA conectadas en este turno. Las evidencias REA anteriores conservan sus IDs. Ghidra directo y las herramientas UPK existentes permiten este mapa sin reinstalar software ni modificar configuración global. No se presenta el rechazo histórico de DLL por REA Windows P0 como fallo de la descompilación directa de Meteor.

Entrada al resto del mapa: [endpoints](HAWKEN_ENDPOINT_CATALOG.md), [autenticación/inventario](HAWKEN_AUTH_PROTOCOL.md), [Master](HAWKEN_MASTER_SERVER_PROTOCOL.md), [partida/replicación](HAWKEN_GAME_SERVER_PROTOCOL.md), [arquitectura](HAWKEN_NETWORK_ARCHITECTURE.md) y [brechas](HAWKEN_UNKNOWN_AND_BLOCKERS.md).
