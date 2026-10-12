# HAWKEN — auditoría de seguridad

Prioridad vigente del usuario: comprobar primero funcionamiento local con su copia Steam; la auditoría amplia y ASan se retoman después. Este registro conserva los resultados y hallazgos anteriores, sin convertir comprobaciones pendientes en PASS. El padre integra, revisa y acepta; dos escritores implementan scopes separados; Luna apoya revisión/compilación y sólo escribe evidencia/logs/build outputs. No se presentan cambios de configuración de agentes como una verificación de su modelo runtime.

Alcance auditado: código propio de la etapa y dependencias fijadas. La primera prueba funcional ejecutó el cliente legítimo, sin capturar tickets/credenciales ni logs del cliente; no constituye una nueva auditoría amplia. Los binarios originales sirven como evidencia y no se suben a scanners externos. Cada capacidad necesita pruebas y controles antes de exponerla; una suite sin fallos no demuestra ausencia absoluta de vulnerabilidades.

## Evidencia actual

| Componente | Control observable | Evidencia / estado |
|---|---|---|
| Integridad M1 | Hash del mismo manifiesto capturado antes de parsearlo; tamaño/hash, lista cerrada, rutas/enlaces rechazados; límites de manifiesto y recorrido | CONFIRMADO como snapshot local estable, sin atestación remota. `m2-cpp-final-green`: 360 UPK coinciden, cero incidencias. |
| INI/CLI M1 | Encoding/URI estrictos, fuente64KiB, duplicados/userinfo/control rechazados; se oculta el path incluso de URI válida | CONFIRMADO en casos positivos/negativos. Preflight exit2, readyForPlayers=false y runtimeVerified=false. |
| Meteor M2 | Tipos/casing, Result sin doble encoding, status HTTP/API/parse independientes, escape por bytes | CONFIRMADO para los contratos puros probados. No concede identidad ni sesión. |
| Codec M2 | Spans/cursor/longitudes, truncado, bound0/overflow, marcador y copia no alineada | CONFIRMADO para el paquete interior probado. No cifra, autentica ni abre un listener. |
| Directorio M2 | Sólo metadatos, dos rutas GET, queries ambiguas rechazadas, errores sanitizados y snapshot1000 registros | CONFIRMADO en diez casos, incluidos HTTP loopback. Sin registro/heartbeat, ocupación live ni admisión. |
| SQL M3 | DB independiente por servidor, transacciones y consultas readonly con columnas autorizadas y recursos limitados | CONFIRMADO internamente en `m3-cpp-final-green`:67 casos/1029 aserciones, cero omisiones. No prueba autenticación Steam, API pública ni permiso query.read del panel. |

Gate funcional actual: `implementation/evidence/client-bridge-functional/final-summary.json`; configure/build/CTest0,194 casos/6392 aserciones en cuatro suites, cero omisiones. Ocho procesos fixture comprueban el puente propio; `master-port80/results.json` comprueba arranque/listado vacío/cierre del Master exclusivamente en127.0.0.1:80. Se registran sólo estados/categorías, sin tickets ni Steam. Los60 inputs compilados/dependencias y los hashes originales/perfil conocido coinciden antes/después. La aceptación se limita a esos mecanismos; cliente Steam, admisión y gameplay siguen pendientes. No es una nueva auditoría amplia. Los gates históricos `local-master-functional` y M2 se conservan; M2 summary SHA-256 `023166c3b56395cd96c9f60179aad08dddfddee122683d270c63a4ead5969812`.

## Hallazgos y resolución

`implementation/evidence/security-review-2026-10-11.json` conserva la revisión inicial (SHA-256 `d79f5b890925e565624954e617f46dd1e5a392d74b459b7f141b572947c9ef3a`). La evidencia RED anterior no se reemplaza con el resultado posterior.

| Hallazgo | Evidencia observada e interpretación | Estado actual |
|---|---|---|
| SEC02, INI sin cota de lectura | `src/meteor_ini.cpp`, lectura: ahora comprueba64KiB antes de cada append; exacto y exceso UTF8/UTF16 ensayados | CONFIRMADO corregido en el gate final; crecimiento concurrente no inyectado. |
| SEC03, manifiesto/árbol sin presupuesto | `src/package_integrity.cpp`:4MiB antes y durante lectura,4096 entradas,16 contenedores JSON, profundidad64 y100000 entradas del árbol; aborto de escaneo antes de hashes de paquetes | CONFIRMADO corregido. Bordes de bytes/entradas del manifiesto/depth probados; borde100001 entradas sólo revisión de fuente. |
| SEC05, URI válida filtraba su path | `src/cli.cpp`, salida preflight: sólo scheme/host/port y basePathRedacted; un sentinel de path no aparece | CONFIRMADO corregido mediante dos regresiones RED→GREEN. |
| MSEC01, catálogo sin límite de registros | `src/master_directory.cpp`, constructor: rechaza más de1000;1000 acepta/1001 rechaza | CONFIRMADO corregido mediante RED→GREEN. Límite propio, no recuperado del servicio original. |
| SEC04, path inspeccionado y abierto separadamente | `src/package_integrity.cpp`: hay intervalo entre inspección y apertura/hash | CONFIRMADO abierto. Requiere árbol local estable; no prueba bytes cargados remotamente ni elimina TOCTOU. |
| SEC01/SEC06/SEC07, futuras fronteras | Codec sin listener; serializer de SessionKey sólo llamado por fixtures; Users vacío evita exponer identidades pero no representa ocupación live | Gates de transporte, secretos y proyección local siguen NO VERIFICADOS. |

Hashes de los fuentes corregidos, fijados por el gate final: `meteor_ini.cpp` `e4d13f0b8da322fb46ba95563ede20b84a0fb19bb1a27ef4e1da40c92ac59970`; `package_integrity.cpp` `8a53bb0144842b1ab44e9ec31067cf517280c9e3cce0de5616f4c477109597e3`; `cli.cpp` `81c67ef460f1e48e8af8e2e17decb9bd5e9d26ec8162f4808df8280894bebf53`; `master_directory.cpp` `c0be52433f84200f3f52d173d5f0b0c752cfd1e00ea66d1bb8059a06be1fed99`. Direcciones/RVA del original corresponden a los informes de RE; estos hallazgos son de fuente propia.

Erratum de evidencia: el gate `m2-security-green-directory-limit-red` comparó tres originales ausentes por paths incorrectos y no demostró su integridad. `SNAPSHOT_ERRATUM.md` allí retira esa interpretación conservando los logs. El gate final comprueba correctamente EXE/SDK/INI/manifiesto y360 paquetes; no convalida retrospectivamente aquella comparación.

## Dependencias e instrumentación

JSON3.12.0, doctest2.4.12, cpp-httplib0.58.0, SQLite3.54.0 y toml++3.4.0 están fijados, sin descargas en build. Hashes/procedencia/licencias: `cpp-dependencies.json`, `cpp-http-dependency.json`, `cpp-sqlite-dependency.json` y `cpp-toml-dependency.json` en evidence. El SHA3-256 del archivo SQLite coincide con el publicado; los SHA-256 extraídos se registran como observaciones locales. La identidad del archivo toml++ se comprobó contra el blob del commit oficial fijado; no se presenta su SHA-256 local como una atestación del editor.

La consulta pública oficial de cpp-httplib devolvió23 advisories, registrada en `cpp-httplib-public-advisories.json` (SHA-256 `812331e9a871e8ef75a48425d1182575e889e00d086e5a1506b8f81c15ea1822`). La versión fijada0.58.0 queda fuera de los rangos afectados publicados revisados, cuyas correcciones llegan a0.51.0. Las listas oficiales de JSON y doctest no mostraron advisories publicados en la revisión. Esto no descarta fallos desconocidos ni garantiza que futuras versiones sigan libres de advisories. Referencias: [cpp-httplib](https://github.com/yhirose/cpp-httplib/security/advisories), [JSON](https://github.com/nlohmann/json/security/advisories), [doctest](https://github.com/doctest/doctest/security/advisories), [SQLite CVEs](https://www.sqlite.org/cves.html).

ASan permanece **NO ACEPTADO**: `m2-cpp-asan-green/summary.json` registra build0 y CTest8; ambas suites abortan al destruir objetos globales pese a pasar sus aserciones. Los probes de `asan-allocator-probe` reproducen el mismatch con un par válido global new[]/delete[] sin doctest: MT y MD fallan; el control desde main pasa; llamar __asan_init no cambia el fallo. El detector continúa habilitado con alloc_dealloc_mismatch=1. La causa exacta de compiler/link/runtime es NO VERIFICADA; no se desactiva el control ni se parchea el vendor para obtener PASS.

`asan-allocator-probe/compiler-runtime-review.json` (SHA-256 `51fc31e6c5edaa1d6a1cba5948575048c668b642355dcba2e1a66802c8c61898`) no encontró otro runtime ASan listo en los compiladores instalados examinados. El runtime MSVC usado tiene SHA-256 `3aa742521e8cbd23252f0545aa51d5a15624a3e4430fbe2f81567afaeb3fc59b`. Esta limitación no se presenta como prueba de seguridad de memoria; seguirá abierta mientras avanzan módulos internos que puedan verificarse con pruebas normales.

## Límites y pendientes concretos

- **Integridad remota no resuelta:** SHA-256 local comprueba archivos leídos en ese instante. No prueba qué bytes carga un cliente remoto ni evita manipular el verificador/una carrera de carga. CLI declara remoteAttestation=false; no se afirma protección anticheat con ese resultado. La autoridad de movimiento/daño/recompensas tendrá que residir en el servidor.
- **Admisión Steam no resuelta:** proceso Steam, SteamID declarado o Status200 no autorizan una cuenta. Hace falta validar ticket con el mecanismo legítimo que corresponda a este productor original y comprobar el resultado antes de crear sesión. Sin tickets/keys en logs o UI.
- **Límites de transporte pendientes:** el codec recibe un buffer interior y un bound explícito del payload; el valor original runtime aún no está confirmado. El futuro listener debe limitar tamaño/volumen/conexiones antes de decodificar; no se añade un guard nativo inventado ni se aceptan datagramas ilimitados en red.
- **Directorio live no resuelto:** Users tiene usos de ocupación y asignación confirmados estáticamente. La proyección vacía del snapshot no prueba el flujo live. Registro/heartbeat remoto aún no existen; requerirán credenciales de instancia y expiración/generación sin permisos sobre cuentas.
- **SQL/Creator aún no expuestos:** el caller futuro debe autorizar query.read y elegir una DB/política confiable de su instancia. Las vistas son comodidad; sólo el allowlist exacto de tabla/columna puede conceder lectura, incluso en CTE/WHERE/ORDER. No se afirma seguridad por filas. Sin ATTACH/extensions/shell; escrituras económicas, auditoría, backup/restauración requieren gates posteriores. Almacenar un SteamID válido en formato no verifica Steam.
- **Plataformas y dependencias:** Windows probado en M1; Linux aún necesita compilación y ejecución nativas. Revisar avisos publicados para versiones fijadas antes de publicar; mantener licencias/hash/procedencia, sin descargas durante build.

Luna revisa y ejecuta los gates asignados por el padre con fuentes congeladas. El padre contrasta la fuente y evidencia y decide aceptación; cada finding abierto se conserva hasta resolución demostrada. Antes de exponer HTTP públicamente faltan TLS, límites de conexiones/cola/tasa y pruebas de carga/errores, además de autenticación de las escrituras futuras. La DLL propia se instaló únicamente durante un ensayo funcional local autorizado, con firewall del EXE limitado a loopback y limpieza verificada; la revisión no autoriza exposición pública ni acredita admisión Steam o gameplay.
