# M1 — lector MeteorUri

Estado inicial: modelos públicos y pruebas preparados; `MeteorIniReader.Read` lanza `NotImplementedException`. La lógica permanece pendiente hasta observar RED mediante el dueño de pruebas. Este escritor no ha compilado ni ejecutado tests.

Ownership: `src/Hawken.Core/Configuration/**`, `tests/Hawken.Foundation.Tests/MeteorIniTests.cs` y este informe. No se modifican proyectos, módulos ajenos, evidencia congelada ni el juego.

La muestra original es plana/sin sección, ASCII con CRLF, 133 bytes: clave `MeteorUri` en línea4, offset0x4d. SHA256 `08eda2cb20539996b7c1a91480c1ee2f64e02743c9bbee0bfc4f3543d8fec3d9`. Se cotejó sólo mediante lectura. Las pruebas usan copias temporales y expectativas literales de línea/bytes, incluyendo UTF8 y UTF16LE/BE con BOM, comentarios, URI base y fallos sanitizados.

Contrato de errores acordado: `missing_key`, `duplicate_key`, `empty_value`, `unsupported_section`, `invalid_encoding`, `invalid_uri`, `read_failed`. EncodingName: `utf-8` (incluye ASCII sin BOM), `utf-16`, `utf-16BE`. Secciones rechazadas conservadoramente; no se propone un parser INI genérico.

Auto-revisión inicial: tests sin mocks, sin red ni escrituras en originales; verifican procedencia y preservación de bytes. El lector pendiente no interpreta ni contacta la URI. Leer una configuración no resuelve la sobrescritura posterior del EXE, no verifica carga/timing del puente DLL ni acredita conexión o admisión Steam. El padre conserva revisión, integración y aceptación.
