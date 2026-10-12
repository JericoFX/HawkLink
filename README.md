# HawkLink

Repositorio del código C++ propio, perfiles de desarrollo, herramientas y documentación de investigación. Los binarios originales de Hawken, paquetes extraídos, instalaciones de herramientas, bases de datos y capturas de ejecución permanecen fuera de Git. Los enlaces a `implementation/evidence` y a los resultados de análisis corresponden a evidencia local; no se distribuyen esos archivos mediante este repositorio.

Los checkpoints conservan trabajo en curso: no implican que el cliente ya pueda jugar una partida compatible. Consultar el estado de implementación y sus limitaciones antes de ejecutar un ensayo.

**Cambio de diseño confirmado (2026-10-11): cuentas, saldo y adquisiciones permanentes serán globales en un servicio central separado del Master. Easy Launch con CLI y asistente gráfico está solicitado. Ver la decisión vigente al inicio de [diseño](implementation/HAWKEN_SERVER_LOCAL_DESIGN.md). La implementación local descrita abajo aún no incorpora estos cambios.**


Reconstrucción propia de los servicios y del servidor de partida de Hawken a partir del cliente Steam instalado. Todo el software propio usa C++20. El diseño vigente está en `implementation/HAWKEN_SERVER_LOCAL_DESIGN.md`: datos y progreso por servidor, Master sólo directorio. Sustituye las propuestas históricas de C#/.NET y progreso central de `research/2026-10-10/HAWKEN_FULL_IMPLEMENTATION_PLAN.md`. El estado y las pruebas están en `implementation/IMPLEMENTATION_STATUS.md`.

Productos previstos: MasterServer que lista destinos por región; servicios separados de identidad Steam, perfiles, asignación y progresión; GameServer autoritativo; Server Creator con panel, diagnóstico de puertos/SQL, mapas, modos, bots, eventos, sanciones, consultas y mensajes. Windows y Linux son objetivos del código propio. El cliente se inicia desde Steam, sin launcher propio.

La identidad y admisión requieren Steam legítimo. Cada servidor guarda su propia SQL con cuentas, créditos, XP, desbloqueos, cosméticos y sanciones; su operador fija reglas y eventos. No se comparte progreso entre servidores y el Master no lo almacena. No hay pagos ni monetización. Nuevos paquetes de contenido quedan aplazados; la primera versión usa una lista cerrada de hashes de paquetes originales.

## Estado verificable

El [gate Windows vigente](implementation/evidence/menu-reliable-native-full-green/parent-acceptance.json) pasó compilación y las 21 suites: 535 casos/19562 aserciones, sin skips. Incluye los seis GET de sincronización restantes del menú, HTTP protegido, diagnóstico sin IDs y el módulo puro de retransmisión. El backend ya integra MenuSync; la retransmisión todavía no tiene listener UDP. Catálogo Recruit de nueve tipos, oferta gratuita e instancias persistidas conservan las reglas locales explícitas. Los 12 casos CLI inválidos se rechazaron sin crear datos. Los cinco originales conservan sus hashes; menú y partida con esta versión siguen NO VERIFICADOS.

En [121321](implementation/evidence/local-services-starter-20261011-121321/parent-runtime-review.json) corrieron backend y Master; las diez consultas del operador pasaron. Steam inicializó el proveedor y la SQL migró a v3, conservando una cuenta, saldos0 y cero grants. Cierre del backend0 y contextos/pipes0; cinco originales intactos. **Esto verifica servicios en ejecución, no una partida ni consumo del inventario por Hawken.** El ensayo con cliente requiere la regla temporal de red del helper elevado.

La prueba previa con cliente [094223](implementation/evidence/local-steam-routing-20261011-094223/parent-runtime-review.json) validó el ticket de la copia Steam: BeginAuthSession, callback143 de20bytes, identidad/respuesta y licencia AppID271290 correctos. El cliente original recibió login200, tres perfiles200 y los cinco GET de saldos, logros, recompensas y conversión200. Aquel backend todavía devolvía404 para el catálogo. La captura de ventana falló también tras recuperar el target; no se atribuye a este ensayo una pantalla ni un código de error. Menú, admisión al GameServer y partida siguen pendientes. El modo local actual admite un solo intento BeginAuth por proceso.

La investigación incluye binarios, paquetes, endpoints y cuerpos recuperados con Ghidra. Una conclusión estática y un fixture del codec no equivalen a una partida compatible. Se reconstruyen las capas por separado hasta comprobar una partida completa con dos clientes originales y cuentas Steam legítimas.

M1–M4 contienen `hawken-tools`, integridad/INI, contratos Meteor, codec interior, directorio Master, SQLite por servidor, consultas restringidas, TOML, arranque HTTP y autenticación Steam local optativa. El [gate Windows conjunto](implementation/evidence/steam-auth-integration-green-dependencies/parent-review.json) pasó nueve suites,249casos/7747aserciones,17checks CLI y ocho procesos fixture, sin omisiones. La reconstrucción corrigió las dependencias MSVC perdidas por un prefijo malcodificado. El wrapper UDP pasó14casos/424aserciones con oracles independientes; todavía no intercambió paquetes con Hawken. El rehash anterior coincidió con los360paquetes originales y se conserva como prueba separada. Linux y el transporte/gameplay continúan sin validar; `readyForPlayers=false` sigue siendo correcto.

La prioridad sigue siendo una partida local real y después Master + servidor + Hawken. En094223 corrieron backend127.0.0.1:80, Master127.0.0.1:18080 y el EXE original con parámetros de ventana1280×720, Steam existente y DLL propia cargada. Se observaron50solicitudes del juego al backend después de descontar cuatro consultas del operador; la única consulta al Master fue del operador. Backend exit0, contextos Steam/pipes0, retirada de archivos/regla temporales y cinco originales intactos. Los diagnósticos contienen categorías e indicadores, sin tickets, grants ni IDs. El Master conserva sólo directorio/regiones. La auditoría amplia y ASan siguen aplazados.

El [almacén v2](implementation/evidence/store-wallet-green/parent-review.json) pasó77casos/1169aserciones, junto con21casos/388aserciones de acceso. La migración v1 transaccional y saldos cero también se observaron en la [SQL propia durante el ensayo](implementation/evidence/local-steam-routing-20261011-094223/sql-after-runtime.json), con respaldo v1 conservado. El [gate HTTP](implementation/evidence/sync-http-green/parent-review.json) pasó54casos/1749aserciones, incluidas sincronización16/815, HTTP protegido, arranque y directorio. Las cinco respuestas200 del juego original están verificadas; la aceptación de los parsers y el menú todavía no.

El destino se configura en `profiles/hawken-client.toml`, que el puente leerá junto a su propia DLL:

```toml
[network]
meteor_uri = "http://127.0.0.1"
```

El lector actual acepta HTTP/HTTPS y un hostname o IPv4, sin puerto explícito ni path. La restricción de puerto sigue el consumidor observado del SDK; el ensayo local usa HTTP/80. Cambiar el destino requiere cerrar el juego y volver a iniciarlo. Es la base de API Meteor, no el endpoint UDP de una partida. El backend responde estado/versión; el modo predeterminado conserva login503 y `--steam-local` habilita la validación Steam comprobada. El Master separado ofrece sólo el listado vacío. Ejecutar ambos procesos no prueba su integración completa con Hawken.

## Compilar y probar

Windows: Visual Studio 2022 Build Tools con MSVC x64, CMake >=3.25 y Ninja. En PowerShell, activar el entorno instalado:

```powershell
$hawkenVs = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools'
Import-Module "$hawkenVs\Common7\Tools\Microsoft.VisualStudio.DevShell.dll"
Enter-VsDevShell -VsInstallPath $hawkenVs -Arch amd64 -HostArch amd64 -SkipAutomaticLocation
cmake --preset windows-msvc
cmake --build --preset windows-msvc
ctest --preset windows-msvc
```

Linux: GCC con C++20, CMake >=3.25, Ninja y las cabeceras/biblioteca OpenSSL 3. El preset `linux-gcc` usa el compilador de ese host. Su existencia no acredita una prueba Linux ejecutada; consultar el estado vigente.

```text
cmake --preset linux-gcc
cmake --build --preset linux-gcc
ctest --preset linux-gcc
```

Las dependencias JSON, doctest, HTTP, SQLite y toml++ están fijadas en `third_party`, con licencias y procedencia en `implementation/evidence/cpp-dependencies.json`, `cpp-http-dependency.json`, `cpp-sqlite-dependency.json` y `cpp-toml-dependency.json`. El código propio es C++20; SQLite es la dependencia C oficial embebida. SHA-256 utiliza CNG en Windows y OpenSSL en Linux. No se descarga nada durante la compilación.

## Diagnóstico local

```powershell
.\build\windows-msvc\hawken-tools.exe verify-packages --game 'F:\SteamLibrary\steamapps\common\Hawken'
.\build\windows-msvc\hawken-tools.exe preflight --game 'F:\SteamLibrary\steamapps\common\Hawken'
```

`verify-packages` devuelve 0 si coincide la lista cerrada, 1 ante fallo y 64 ante argumentos inválidos. `preflight` devuelve 2 mientras la admisión y partida no estén verificadas. El JSON explica cada condición; una URI válida no produce un estado conectado.

La baseline incluida contiene los 360 paquetes de la muestra investigada y está fijada por SHA-256 del manifiesto. Es una baseline observada localmente, sin firma de Valve. Un manifiesto alternativo requiere `--manifest` y `--manifest-sha256` explícitos. Se comprueban bytes actuales, tamaño, faltantes, extras y rutas/enlaces; no se autorreparan ni aprueban paquetes nuevos. La verificación representa una instantánea local con el árbol estable, no una atestación remota ni una solución de la carrera entre verificar y cargar.
