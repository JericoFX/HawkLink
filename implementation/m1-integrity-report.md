# M1 — integridad de paquetes

Primera entrega TDD: modelos y API bajo `src/Hawken.Core/Integrity/`, tests en `tests/Hawken.Foundation.Tests/PackageIntegrityTests.cs`. `Verify` lanza `NotImplementedException`; no contiene lógica de verificación todavía.

Los tests usan archivos y manifiestos pequeños en carpetas temporales independientes de la instalación. Cubren paquetes intactos, alteraciones de hash/tamaño, faltantes y extras recursivos, archivos ajenos permitidos, confianza del manifiesto, metadatos inválidos, paths absolutos/traversal/collisiones, campos históricos y enlaces. Los tests de enlaces tienen skip explícito si la plataforma no permite crearlos.

Estado: no ejecuté compilación ni tests. Pendiente RED del dueño externo; la implementación esperará su confirmación. No se afirma PASS ni integridad real del juego.

Auto-revisión: cambios limitados al ownership asignado; SHA esperado del paquete de fixture es un literal independiente. El hash confiable del manifiesto de fixture se calcula sobre sus bytes antes de cualquier alteración. No hay acceso a originales ni tráfico de red.

Límites previstos: lectura de una instantánea local; no attestation remota, firma oficial ni garantía de bytes durante una carga posterior. La aceptación corresponde al padre tras revisión y validación externa.
