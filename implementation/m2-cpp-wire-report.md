# M2 — codec físico interior C++

Estado: lógica del codec implementada y congelada para GREEN. El padre confirmó RED con configure/build exit0; 79 casos (49 PASS/30 FAIL), 537 aserciones (488 PASS/49 FAIL), stubs esperados e inputs congelados intactos, en implementation/evidence/m2-cpp-red/doctest.log y red-summary.json (SHA256 9fde4af1fda67d51ca8d9a7da2ff30a545b240ef2cbadb35335b392977caa757), y autorizó implementación. Los 18 TEST_CASE propios no se modificaron. Este escritor no ha compilado ni ejecutado pruebas. El padre integra y acepta; Luna posee builds/tests. GREEN todavía pendiente.

Archivos propios: include/hawken/game_wire.hpp, src/game_wire.cpp, tests/game_wire_tests.cpp y este informe. M1 local está aceptado; se actualizó únicamente su informe con 49/49 casos y 488/488 aserciones. APIs ajenas, investigación, archivos originales y C# archivado se preservan. Diseño vigente: SQL/economía por servidor, Master sólo directorio/regiones; el codec no concede progreso ni implementa esa arquitectura.

Se cotejaron cuerpos decompilados completos con SHA256 del EXE original 4a861ecb4ac7add9554f8daf8f98ad2c12b9cb6c1865a83e8be808beb46ea8a2. ImageBase preferida 0x140000000; código citado tiene offset original = RVA - 0xc00. Fuentes relativas a research/2026-10-10/evidence:

| Fuente | RVA / offset original | Hecho físico que ancla las pruebas |
|---|---|---|
| ghidra-main-bitreader-methods.jsonl y bitreader-vtables.json | 0x0804d0 / 0x07f8d0; máscaras RVA0x1adf3e8 / offset0x1ade7e8 | Lectura con máscaras 01 02 04 08 10 20 40 80, cursor en bits, salida puesta a cero y error antes de copiar fuera del buffer. |
| ghidra-main-bitreader-methods.jsonl | 0x083e40 / 0x083240 | Entero acotado: valor0/máscara1; duplica máscara y continúa mientras valor+máscara<límite. No ceil(log2) universal. |
| ghidra-main-bitreader-atend.jsonl | 0x06d7d0 / 0x06cbd0 | AtEnd es error o posición==longitud. El probe no_function anterior quedó resuelto en esta fuente. |
| ghidra-main-packet-receiver.jsonl | 0x9ef440 / 0x9ee840 | Tras unwrap, último byte no cero; su bit activo más alto es marcador y no pertenece al bitlength entregado al reader. |
| ghidra-main-final-reader.jsonl | 0x9ee9e0 / 0x9edde0 | ID acotado0x4000; selector ACK; rama bunch con flags condicionales, canal0x800, secuencia0x400, tipo8 y longitud acotada connection+0x180 multiplicada por8. |
| packet-reader-instructions.json | 0x9eeb27 / 0x9edf27; call0x9eeb2f / 0x9edf2f | MOV EDX,0x4000 (ba00400000) fija bound ACK que el pseudocódigo omite. |
| ghidra-main-final-bits.jsonl | 0x074de0 / 0x0741e0; 0x086050 / 0x085450 | Primer helper sólo consulta cursor: no alineación wire. Segundo reserva (bits+7)>>3 y copia bits. |
| ghidra-main-bunch-delivery.jsonl | 0x087340 / 0x086740; 0x9ef250 / 0x9ee650 | Copia con offsets en bits/preservación de bordes; entrega/cola fiable existen pero no se implementan en M2. |

Vectores literales independientes del writer, LSB primero y posiciones base0:

| Bytes literales | Desglose | Resultado esperado |
|---|---|---|
| 95 | Bits 1,0,1,0,1,0,0,1 | Lectura de 8 bits literal. |
| 02 con bitlength3 / 09 con bitlength4 | Límite10: 2 consume3 bits; 9 consume4 | Entero con ancho dependiente del valor parcial. |
| 23 41 1a 21 | packet0x123[0..13], ACK1[14], ack0x234[15..28], marcador[29]. Entero packed0x211a4123. | Un ACK, sin campos añadidos. |
| 05 80 1d c0 01 2a d4 | packet5[0..13], selector0[14], control1[15], open1[16], close0[17], reliable1[18], canal3[19..29], secuencia7[30..39], tipo2[40..42], longitud5 bound128[43..49], payload0x15[50..54], marcador[55]. | Un bunch con payload5 bits, max_packet_bytes=16. |
| 03 00 02 00 08 | packet3[0..13], selector/control/reliable0[14..16], canal1[17..27], longitud0 bound128[28..34], marcador[35]. | Sin bits Open/Close/Sequence/Type cuando no corresponden. |

Las pruebas también cubren EOF/cursor/error, constructor fuera del span, copia no alineada/padding0, límites0/1/uint32 máximos, writer con marcador parcial/múltiplo8 sin mutar datos, valores fuera de rango, paquetes truncados, varios registros consecutivos y longitud8/bound24 que consume4 bits. Fixtures complejos usan writer sólo después de los anclajes literales. El writer es el inverso requerido por el contrato nuevo; no se atribuye su cuerpo al emisor nativo ni a una captura runtime.

Códigos aprobados por el padre: truncated, invalid_limit, invalid_length, invalid_value e invalid_marker. La API rechaza límite0 y overflow de max_packet_bytes*8 conservadoramente; no copia el overflow uint32 del nativo. max_packet_bytes provee el bound dinámico del campo longitud, cuyo valor runtime original no se conoce. El cuerpo 9ef440 no demuestra un guard global datagram.size<=connection+0x180: no se añade uno por suposición. Un futuro listener deberá definir su política de tamaño aparte.

Disposición física CONFIRMADA estáticamente; nombres semánticos Open/Close/Reliable/PacketId siguen PROBABLES. El codec devuelve IDs wire módulo0x4000/0x400 sin reconstruir ventanas con estado de conexión. Conserva tipos físicos0..7 sin inferir admisión/canales implementados. No implica criptografía, transporte/socket, handshake, Steam válido, ACK/retransmisión operacional, actores, replicación ni gameplay compatible. No se modifican UNKNOWNs históricos ni se promete efectividad del INI.

Implementación: BitReader toma un span prestado y mantiene cursor/error; sus límites de copia se comprueban antes de acceder o reservar salida y el padding queda cero. Reader y writer usan máscara/prefijo de 64 bits para decisiones acotadas sin overflow de uint32, con valor/prefijo inicial0. BitWriter valida entrada, inicializa cada byte a cero y construye el marcador sobre una copia, sin modificar bits al consultar packet_bytes. decode_packet elimina sólo el marcador final, conserva el orden físico/condicional recuperado y devuelve IDs raw sin ventanas ni políticas de tipo. Checks de capacidad usan divisiones/restos en lugar de bytes*8 no comprobado. La suma de tamaño/posición y max_packet_bytes*8 se validan antes de multiplicar/avanzar. No hay sockets ni dependencias nuevas.

Auto-revisión completa por lectura de API/implementación/tests y cuerpos exactos: cursor<=bit_count, máscaras como máximo2^32 para límites uint32, payload acotado por bits disponibles antes de reservar, marcador basado en bit activo más alto y progresión del cursor en cada registro. Vectores y sus offsets se desglosan arriba y en comentarios de tests. Inputs congelados para GREEN; no hay resultado runtime del codec ni aceptación M2 todavía.
