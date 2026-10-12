# M3 — consultas locales de Creator, lógica para GREEN

Estado: arreglo del authorizer implementado y congelado para nuevos gates normal/LLVM. El padre observó y revisó RED en implementation/evidence/m3-cpp-red: configure0/build0/CTest8, storage2/56 PASS y54 FAIL por stubs, 278/364 aserciones PASS y86 FAIL, foundation90/master10 PASS; gate-summary.json registra inputs/originales sin cambios. El primer gate posterior, implementation/evidence/m3-cpp-green, falló: configure0/build0/CTest8, storage53/60 PASS, 929/932 aserciones PASS; siete casos de consultas fallidos y ningún cambio entre snapshots de49 inputs/originales. No se aceptó M3. El padre autorizó el arreglo después del probe sintético ejecutado por Luna. Los33 casos previos conservan sus expectativas; se añaden siete regresiones (40 TEST_CASE propios actuales), sin RED separado de esas nuevas regresiones. Este escritor no ha compilado ni ejecutado pruebas. Luna posee builds/tests y el padre integra/revisa/acepta; nuevos GREEN y auditoría siguen pendientes.

Scope propio: include/hawken/creator_query.hpp, src/creator_query.cpp, tests/creator_query_tests.cpp y este informe. CMake, SQLite, LocalStore y documentos comunes pertenecen a otros escritores. No se han modificado originales del juego ni entradas congeladas M1/M2. Diseño vigente: DB/cuentas/progreso por servidor; Master sólo directorio y regiones.

## Contrato acordado

Namespace hawken::creator. QueryBind admite null/int64/texto; QueryCell añade double. ReadColumn, QueryLimits, QueryPolicy y QueryResult conservan los tipos propuestos. QueryError tiene enum y mensaje fijo «Creator query could not be executed.», sin SQL, valores, ruta ni detalle SQLite. deadline_exceeded distingue el reloj de resource_limit para otras cuotas.

El path y la política son entradas confiables del caller después de comprobar query.read y alcance local. La API no autentica Steam ni acredita permisos del panel. Permite todos los rows de esa instancia en las columnas concedidas; las vistas son comodidad, no una barrera de seguridad por fila. READ de columnas no vacías exige (main, tabla, columna) exacta, comparando identificadores ASCII case-insensitive con sqlite3_stricmp e incluyendo grants separados para vista y tablas base. El sexto argumento de authorizer nunca concede acceso. Política con hasta256 entradas y nombres ASCII de identificador simple hasta128 bytes, con excepción de columna vacía; sqlite_* se deniega siempre.

SQLite3.54 emite también READ(tabla,"",DB) sintético para acceso sólo a filas y para referencias a INTEGER PRIMARY KEY que no marcan colUsed. DB puede ser NULL si el FROM no se calificó. Sólo ese evento vacío puede omitir main en nuestra conexión privada sin otros destinos: requiere grant vacío explícito O una lectura main de columna no vacía ya autorizada de esa misma tabla en esta consulta. Un grant no usado, otra tabla o nombre de alias/CTE/source no conceden ese acceso. El grant vacío permite recuento/sólo filas cuando no se ha autorizado ninguna columna real. El callback no distingue COUNT(*) con predicado autorizado de COUNT(columna); ambos pueden ser permitidos por la lectura real previa. No es autorización por forma textual de SQL. COUNT(*) desnudo sigue denegado sin grant vacío, aun si la política contiene columnas de esa tabla.

Una conexión privada READONLY nueva por consulta, sin creación ni handles externos; defensive activo, trusted_schema desactivado, prepare NO_VTAB/DONT_LOG y stmt_readonly sólo como respaldo. Authorizer instalado hasta finalize, incluyendo reprepare; por defecto deniega writes, ATTACH/DETACH, PRAGMA, transacciones, DDL, recursión, vtables, esquema y funciones ajenas a la lista. Whitelist inicial: count, sum, min, max, avg, coalesce, lower, upper, length y abs. El target del padre omite load_extension: no se llamará sqlite3_enable_load_extension.

Exactamente una sentencia efectiva no EXPLAIN, sin NUL en SQL; pzTail y prepares adicionales comprueban toda la cola antes del primer step. El parser SQLite puede ignorar comentarios, whitespace y sentencias vacías ';'; una segunda sentencia efectiva se rechaza sin ejecutar la primera. No se divide SQL ni se introduce un lexer propio. Authorizer/progress permanecen activos durante ambos prepares. Sólo parámetros ? anónimos con cantidad exacta; texto se pasa con longitud explícita y no se interpola. Resultados BLOB se rechazan por contrato, sin conversión implícita a texto. NUL dentro de binds o resultados TEXT se conserva con longitud explícita.

| Límite | Default | Techo interno aceptado |
|---|---:|---:|
| SQL, bytes originales | 16384 | 1048576 |
| Filas | 100 | 10000 |
| Bytes de resultado | 262144 | 8388608 |
| Callbacks de progreso | 1000 | 100000 |
| Deadline cooperativo, ms | 100 | 5000 |

Todos los valores son positivos. Texto total de binds <=max_sql_bytes. Bytes de resultado = nombres de columnas + bytes de TEXT +8 por int64/double; null0. Se cobran metadatos aunque no haya rows. Es una medida de payload, no del heap total. El presupuesto restante se comprueba antes de copiar nombres/TEXT en strings o celdas; se resta sin overflow. Exceso/errores descartan el resultado parcial. SQLite puede convertir/obtener TEXT internamente antes de la comprobación; no se afirma ausencia de esa asignación del motor.

Límites complementarios SQLite: LENGTH16MiB, SQL_LENGTH=max(64KiB,max_sql_bytes), COLUMN256, EXPR_DEPTH100, COMPOUND_SELECT32, VDBE_OP100000, FUNCTION_ARG32, VARIABLE_NUMBER1024, ATTACHED0, TRIGGER_DEPTH0 y WORKER_THREADS0. SQL y binds de aplicación se comprueban aparte; el floor de SQL y límite LENGTH permiten cargar el schema confiable incluso cuando max_result_bytes=13 o el SQL permitido es corto. No son límites de heap global ni un nuevo contrato del cliente Hawken.

Deadline monotónico desde antes de abrir conexión, comprobado antes/después de prepare/step y al terminar cada row. Progress handler nominal cada1000 instrucciones VM comparte el presupuesto entre prepares y ejecución; permite la cantidad configurada de callbacks y aborta el siguiente, comprobando primero el reloj. Ese intervalo no cuenta exactamente todo el trabajo de CPU. No garantiza un límite duro para I/O bloqueado o memoria total. busy_timeout0 evita una espera de lock configurada. No se habilitan extensiones.

## Pruebas independientes preparadas

Fixture SQL privada con accounts, visits, bans, roles, números y vistas de comodidad; se crea mediante SQLite fuera de la API bajo prueba y se compara el archivo antes/después de consultas/rechazos. No usa ni impone el esquema del LocalStore ajeno.

- Lectura de vista, directa y CTE no recursiva; tipos/UTF-8; grants separados base/vista; roles denegados hasta grants explícitos; count(*) desnudo con columna vacía explícita.
- SteamID/secretos/roles restringidos en SELECT, WHERE, ORDER, GROUP, agregados, subconsultas y alias; CTE que suplanta el nombre de una vista permitida; sqlite_schema/master aun con grants erróneos.
- Rechazo de writes/DDL/temp/attach/detach/pragma/transacciones/VACUUM/recursión y funciones no permitidas. load_extension y pragma_table_info como vtable se rechazan en compilación (invalid_sql) por las restricciones de build/NO_VTAB; no se fuerza una clasificación authorizer que quizá no ocurra.
- Una sentencia efectiva con comentarios, punto y coma dentro de literal y cola con sentencias vacías ';'; segunda sentencia/cola inválida/SQL vacío/NUL/EXPLAIN; binds null/int64 extremos/inyección literal/NUL, cantidad exacta y rechazo de nombres o números; texto que parece parámetro dentro de literal.
- Límites exactos y exceso: SQL en bytes, binds acumulados, filas, metadatos/texto UTF-8/NUL/números/null y cero rows; política/techos inválidos y techos exactos válidos. Producto cartesiano de200^4 candidatos: callbacks1/timeout5000ms espera resource_limit; callbacks100000/timeout1ms espera deadline_exceeded. No se afirma un máximo de latencia de I/O.
- DB ausente sin creación, archivo inválido sin alteración; errores de prepare/step sanitizados; BLOB rechazado.

RED observado con stubs distinguió fallas esperadas de compilación/framework. No se han relajado pruebas existentes. El caso añadido de cola ';' no tuvo RED separado; especifica la decisión interna del padre y será validado con GREEN.

Las siete regresiones nuevas conservan COUNT(*) desnudo calificado/no calificado denegado sin grant vacío; permiten la clave primaria/aggregate sin grant de filas y COUNT con predicado de columna permitida; deniegan contar/join otra tabla y grants no usados, secretos/CTE aun tras una lectura de IPK permitida y sqlite_* con grant de filas. Una consulta posterior empieza sin marcas de la anterior. No se ejecutaron esos nuevos casos todavía ni se afirma un RED propio de ellos.

## Diagnóstico y diff del arreglo

implementation/evidence/m3-query-diagnostic/run-msvc/run.stdout.log contiene el probe de15 consultas/73 eventos del authorizer; compilación/ejecución por Luna terminaron exit0. Es una fixture sintética en memoria, no ejecución del producto ni política productiva: el probe permite SELECT/READ/FUNCTION para observar sus callbacks. COUNT(*) sin calificar emite READ(accounts,"",NULL); calificado usa main. IPK/aggregate/JOIN/EXPLAIN plan emiten primero READ de la columna real main y después el evento vacío; no hay una acción adicional de EXPLAIN. Los scalars de la whitelist pasan en la traza: el fallo de aquel TEST_CASE aparece en su agregado de IPK. Es la causa documentada de los siete fallos de consultas que motivan este arreglo.

Baselines guardados por el padre: creator_query-before.cpp, creator_query-before.hpp y creator_query_tests-before.cpp bajo m3-query-diagnostic. Diff de producción: añadir array<bool,256> en QueryContext; marcar sólo un grant real main usado; aceptar READ vacío con main/NULL exclusivamente mediante grant vacío o marca de la misma tabla; no cambiar los demás actions, errorcodes, guards, binds ni budgets. Diff de header: precisar acceso a columnas/filas en lugar de prometer un permiso por sintaxis COUNT. Diff de tests: sólo añadir siete casos, conservando íntegros los33 anteriores. Este informe registra fallo observado, evidencia causal y nueva limitación del contrato.

## Auto-revisión de fuente

Callbacks noexcept con catch-all y argumentos de strings NULL-safe; desconocidos denegados por defecto, sin SQLITE_IGNORE ni concesión por contexto. No leen SQL ni imprimen valores. Contexto construido antes de la conexión y destruido después: statements se finalizan con RAII antes de cerrar la conexión, manteniendo authorizer/progress instalados hasta el final incluso en exceptions/reprepare. Sólo SQLite conserva los handles, no se exponen al caller. El path se convierte a absoluto UTF-8, sin NUL, para impedir interpretación file:URI; open usa READONLY|PRIVATECACHE y verifica db_readonly(main). No se crea una DB ausente.

Las marcas usan un array fijo inicializado a false, indexado por las hasta256 entradas validadas de política; el callback no asigna memoria. Sólo una lectura autorizada no vacía de main marca su índice. La excepción para DB NULL está limitada a columna vacía y no modifica la comprobación de columnas reales. Estado vivo durante toda la consulta, incluidos prepares de cola y eventual reprepare, sin compartirse entre llamadas. Una segunda sentencia efectiva nunca se ejecuta y todo nuevo READ privado se sigue comprobando contra el grant exacto. No se introducen hooks ni se da por probado reprepare. SQLite no distingue su evento vacío sintético de una columna real llamada "" (sqlite3.c157603-610); el esquema confiable de LocalStore no admite tales nombres, condición necesaria para interpretar esa concesión como acceso sólo a filas.

Nombres de resultados se obtienen después del primer step (también DONE sin filas), para reflejar un eventual reprepare antes de producir rows. Celdas se cobran antes de copiarlas; row limit se verifica antes de construir la siguiente fila. Conversiones int se apoyan en techos validados y tamaño de columnas/params limitado por SQLite. Error SQLite, bad_alloc y otras exceptions se traducen a errores fijos, sin errmsg/SQL/ruta. Las propiedades anteriores son revisión de fuente, no resultados runtime ni prueba de reprepare.

## Cobertura pendiente

Los casos preparados distinguen reloj y presupuesto de progreso por código esperado, pero no prueban por sí solos que authorizer cubra reprepare. No se añadió un hook sólo para tests. Para probar reprepare de forma determinista habría que acordar un seam interno entre prepare y primer step: el fixture cambiaría la definición de una vista mediante otra conexión y comprobaría que el reprepare deniega la nueva columna privada. Eso requeriría autorización previa del padre; no es parte de esta API ni de esta entrega. No se afirma cobertura de ese escenario ni resultados del caso de reloj antes de que Luna lo ejecute.

Faltan GREEN integrado y auditoría obligatoria; reprepare directo continúa sin fixture. No se implementan endpoint/UI, mensajes administrativos, permisos Steam/panel, acceso de clientes a SQL, gameplay ni compatibilidad de MeteorUri.

Fuentes primarias consultadas para el contrato, no para afirmar resultados: [SQLite security](https://www.sqlite.org/security.html), [authorizer](https://www.sqlite.org/c3ref/set_authorizer.html), [stmt_readonly](https://www.sqlite.org/c3ref/stmt_readonly.html), [prepare](https://www.sqlite.org/c3ref/prepare.html) y [progress handler](https://www.sqlite.org/c3ref/progress_handler.html). Header local SQLite3.54.0 del target del padre.
