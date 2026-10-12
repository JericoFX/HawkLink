# M1 Integrity — C++ definitivo

El usuario corrigió a C++ para TODO y confirmó reconstrucción completa Master/GameServer desde clienteSteam. No source ni dedicado adicional. No esperar ese material; primer módulo preparación debe ser seguro y real, sin afirmar compatibilidad.

Ownership auth_endpoints: include/hawken/package_integrity.hpp, src/package_integrity.cpp, tests/package_integrity_tests.cpp y implementation/m1-cpp-integrity-report.md. No estás solo; preservar/acomodar cambios ajenos. No Git/worktrees/delegación/builds/tests. Padre provee SHA, CLI, CMake, JSON y doctest. Usa C++20, nlohmann3.12.0 vendorizado, doctest2.4.12. No nuevas dependencias.

API namespace hawken: struct IntegrityIssue {std::string code,relative_path,detail;}; struct PackageIntegrityReport {bool passed=false;std::string manifest_sha256;std::size_t verified_count=0;std::vector<IntegrityIssue> issues;}; PackageIntegrityReport verify_packages(const std::filesystem::path& game_root,const std::filesystem::path& manifest_path,const std::string& trusted_manifest_sha256). Padre aporta hawken/sha256.hpp: sha256_file(path)->hexlower y sha256(span<constuint8_t>)->hexlower. No implemente criptografía en tu módulo.

Aplican los requisitos/tests funcionales del brief C# archivado implementation/superseded-csharp-2026-10-10/m1-integrity-brief.md, adaptados a C++. Baseline existente original-upk-sha256-manifest.json, schema/hash_algorithm/package_count/entries y lectura actual, sin fiarse de observed ni matches. Path Windows normalizado a plataforma, reject traversal/absolute/collisiones/duplicates, tamaño/hash, missing/extra, links/reparse sin recorrer destinos, lectura fallida siempre FAIL. Lista cerrada sólo .upk/.u/.udk. No escrituras ni autorreparación ni aprobar hashes nuevos. Instantánea local, no attestation remota o carrera hash→carga resuelta.

Reusar códigos de tus tests previos, uppercase. Fixtures temporales reales; helper tests/test_support.hpp padre dispone TempDirectory.path()/write(relative,stringbytes), read_bytes(path). Uso de enlaces con permisos limitados registra caso no ejecutado explícito, no fingir PASS efectivo. Si necesitas ajustar helper, pedir al padre; no editarlo.

TDD: primero header/modelos + verify_packages que sólo lance std::logic_error("not implemented") y tests. Avise y espere RED de Luna/padre antes de lógica. Reuse casos funcionales, no traduzca mechanically C# sin considerar filesystem portable. Ejecución/compilación única Luna cuando padre asigne; no compilar tú. Reportar auto-review y límites.
