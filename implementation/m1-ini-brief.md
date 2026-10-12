# M1 — lector de configuración MeteorUri

Alcance aprobado por «Comienza»: implementar lectura segura, sin editar el INI ni iniciar el cliente. La build original lee MeteorUri y un setter EXE posterior la sobrescribe; leer una URI no acredita que se utilice. El puente DLL sigue pendiente de evidencia de carga/timing y de runtime.

Ownership del escritor game_master: únicamente src/Hawken.Core/Configuration/**, tests/Hawken.Foundation.Tests/MeteorIniTests.cs y implementation/m1-ini-report.md. No editar csproj, otros módulos, perfiles, investigación ni Git. No estás solo: preservar y acomodar otros cambios. No delegar ni ejecutar builds/tests compartidos; el dueño asignado ejecutará pruebas. .NET 10 estándar y xUnit.

API pública: namespace Hawken.Core.Configuration; static MeteorIniReader.Read(string path) devuelve MeteorIniSetting con Uri (System.Uri), LineNumber (int, base1), ByteOffset (long, inicio de la clave en el archivo incluyendo BOM) y EncodingName (string). MeteorIniException indica fallo mediante Code (string); mensajes sanitizados, sin eco de valores rechazados ni contenido completo del INI.

Leer el INI plano original en Binaries/Win64/StormConsumerSettings.ini y cotejar formato antes de escribir. Soportar ASCII/UTF-8 estricta sin BOM, UTF-8 BOM, UTF-16LE/BE con BOM; conservar archivos intactos. Buscar clave MeteorUri sin distinción de mayúsculas, con whitespace alrededor del igual, ignorando líneas vacías/comentarios completos ;/#. Clave ausente, duplicada, valor vacío, encoding inválido y sección ambigua/no soportada deben fallar explícitamente. No inventar interpretación de INI genérico distinta de la que admite la muestra. No ejecutar interpolación de variables, argumentos, red ni escrituras.

URI absoluta HTTP/HTTPS, host presente, puerto válido; admitir path base. Rechazar userinfo, query y fragment porque no pertenecen a este contrato de URI base y podrían introducir secretos; error sanitizado. No registrar contraseñas ni tokens. Detectar otros esquemas, relativa o URI mal formada. Mantener números de línea/offset correctos con LF/CRLF y BOM. No llamar a servicios ni afirmar que un localhost sea accesible.

TDD: tests sobre archivos temporales reales: formato de la muestra, case/espacios, BOM/UTF16 y offset, CRLF/LF, ausente/duplicada/vacía, URI relativa/esquema inválido, userinfo/query/fragment sin fuga al error, encoding inválido, archivo intacto antes/después. Primer estado sólo tests/modelos/esqueleto Read que lance NotImplementedException. Avisar al padre y esperar RED antes de implementar lógica. No compilar tú. Leer writing-good-tests.md antes de escribir.

Informe breve: archivos, contrato observado, limitaciones de la lectura frente al override y auto-revisión. El padre revisa/integra/acepta.
