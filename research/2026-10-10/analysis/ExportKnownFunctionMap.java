// @category Hawken.StaticResearch
// Read existing database metadata only: no analysis, disassembly or decompiler.
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;
import com.google.gson.Gson;
import com.google.gson.GsonBuilder;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.Map;

public class ExportKnownFunctionMap extends GhidraScript {
    protected void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length != 2) throw new IllegalArgumentException("expected-sha256 output-jsonl");
        String sha = currentProgram.getExecutableSHA256();
        if (sha == null || !sha.equalsIgnoreCase(args[0]))
            throw new IllegalStateException("Existing database original identity mismatch");
        Path output = Path.of(args[1]);
        if (Files.exists(output)) throw new IllegalStateException("Retained output already exists");
        Gson gson = new GsonBuilder().serializeNulls().create();
        long count = 0;
        long edges = 0;
        try (var writer = Files.newBufferedWriter(output, StandardCharsets.UTF_8)) {
            var functions = currentProgram.getFunctionManager().getFunctions(true);
            while (functions.hasNext()) {
                monitor.checkCancelled();
                Function function = functions.next();
                Map<String, Object> record = new LinkedHashMap<>();
                record.put("program", currentProgram.getName());
                record.put("program_sha256", sha);
                record.put("image_base", currentProgram.getImageBase().toString());
                record.put("entry", function.getEntryPoint().toString());
                record.put("function", function.getName(true));
                record.put("signature", function.getSignature().toString());
                record.put("body_ranges", function.getBody().toString());
                record.put("body_bytes", function.getBody().getNumAddresses());
                record.put("is_thunk", function.isThunk());
                record.put("is_external", function.isExternal());
                var callees = new ArrayList<Map<String, Object>>();
                for (Function callee : function.getCalledFunctions(monitor)) {
                    callees.add(Map.of("entry", callee.getEntryPoint().toString(),
                                       "name", callee.getName(true),
                                       "is_external", callee.isExternal()));
                }
                record.put("callees", callees);
                record.put("state", "CONFIRMADO_METADATA_BASE;NO_VERIFICADO_SEMANTICA_RUNTIME");
                writer.write(gson.toJson(record));
                writer.newLine();
                count++;
                edges += callees.size();
            }
        }
        println("HAWKEN_MAP sha256=" + sha + " functions=" + count + " call_edges=" + edges);
    }
}
