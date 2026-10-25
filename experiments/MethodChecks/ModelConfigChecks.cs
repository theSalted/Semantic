using System.Diagnostics;
using System.Net;
using System.Text;
using System.Text.Json;

static class ModelConfigChecks
{
    public static async Task Run(string root)
    {
        int count = 0;
        void Check(bool value, string name) { if (!value) throw new Exception(name); count++; }
        void Reject(Action action, string name)
        {
            try { action(); } catch (ArgumentException) { count++; return; }
            throw new Exception(name);
        }
        async Task RejectAsync(Func<Task<MethodReply>> action, string name)
        {
            try { await action(); } catch (InvalidOperationException) { count++; return; }
            throw new Exception(name);
        }
        Check(LocalModel.Resolve(null) == LocalModel.FourB, "C# defaults to original 4B");
        Reject(() => LocalModel.Resolve("2507"), "Unknown model must not silently substitute");
        Reject(() => LocalModel.FourB.ModelPath(root, LocalModel.FourteenB.FileName), "Mismatched explicit path rejected");
        foreach (var model in new[] { LocalModel.FourB, LocalModel.FourteenB })
        {
            Check(LocalModel.Resolve(model.Size) == model, "Explicit size resolves");
            Check(LocalModel.FromPath(model.ModelPath(root)) == model, "Legacy constructor path retains model identity");
            foreach (var launcher in new[] { "scripts/run-local-llm.sh", "unity-host/run-mac.sh" })
            {
                var start = new ProcessStartInfo("/bin/bash") { RedirectStandardOutput = true, RedirectStandardError = true };
                start.ArgumentList.Add(Path.Combine(root, launcher));
                if (launcher.StartsWith("unity-host")) start.ArgumentList.Add("/unused-config-only-project");
                // Omission must select 4B; 14B requires an explicit argument.
                if (model == LocalModel.FourteenB) { start.ArgumentList.Add("--llm-size"); start.ArgumentList.Add(model.Size); }
                start.ArgumentList.Add("--print-config");
                using var process = Process.Start(start)!;
                var output = await process.StandardOutput.ReadToEndAsync();
                var error = await process.StandardError.ReadToEndAsync();
                await process.WaitForExitAsync();
                Check(process.ExitCode == 0, $"Launcher resolution failed: {error}");
                var settings = output.Trim().Split('\n').Select(line => line.Split('=', 2)).ToDictionary(x => x[0], x => x[1]);
                Check(settings["size"] == model.Size && settings["alias"] == model.Alias && settings["file"] == model.FileName,
                    "Launcher and C# select identical model");
                Check(settings["sha256"].Equals(model.Sha256, StringComparison.OrdinalIgnoreCase) && settings["port"] == model.Port.ToString(),
                    "Launcher and C# verify identical hash and endpoint");
                Check(settings["model"] == model.ModelPath(root), "Normal launch resolves repository model path");
            }
            var fake = new ModelHost(model);
            using var methods = new LocalMethods(new("Test", "Concise", "", "", []), model.ModelPath(root), fake, compactHybrid: true);
            var input = new MethodInput("hybrid", "hello", "test", [], [new("0", "Hello.")]);
            var reply = await methods.Run(input, CancellationToken.None);
            Check(reply.Response == "Hello." && reply.Provenance == "selected", "Configured adapter preserves exact authored selection");
            Check((string)reply.Metadata["model"]! == model.Alias && fake.RequestModel == model.Alias && fake.LastPort == model.Port,
                "Request and response provenance identify selected model and endpoint");
            fake.Alias = "wrong-model";
            await RejectAsync(() => methods.Run(input, CancellationToken.None), "Wrong server alias must fail");
            fake.Alias = model.Alias; fake.FileName = "wrong.gguf";
            await RejectAsync(() => methods.Run(input, CancellationToken.None), "Wrong server file must fail");
        }
        var dir = Path.Combine(root, "artifacts", "model-config", Guid.NewGuid().ToString("N")); Directory.CreateDirectory(dir);
        var invalid = Path.Combine(dir, LocalModel.FourB.FileName); File.WriteAllText(invalid, "not model weights");
        var rejectedHost = new ModelHost(LocalModel.FourB);
        using var rejected = new LocalMethods(new("Test", "Concise", "", "", []), invalid, rejectedHost, compactHybrid: true);
        await RejectAsync(() => rejected.Run(new("hybrid", "hello", "test", [], [new("0", "Hello.")]), CancellationToken.None), "Changed weights must fail");
        Check(rejectedHost.LastPort == 0, "Invalid hash fails before network access");
        Console.WriteLine($"{count} model configuration/adapter checks passed; fake HTTP only, no inference.");
    }

    sealed class ModelHost(LocalModel model) : HttpMessageHandler
    {
        public string Alias = model.Alias, FileName = model.FileName;
        public string? RequestModel;
        public int LastPort;
        protected override async Task<HttpResponseMessage> SendAsync(HttpRequestMessage request, CancellationToken ct)
        {
            LastPort = request.RequestUri!.Port;
            object response;
            switch (request.RequestUri.AbsolutePath)
            {
                case "/v1/models": response = new { data = new[] { new { id = Alias } } }; break;
                case "/props": response = new { model_path = FileName, default_generation_settings = new { n_ctx = 8192 }, build_info = "fake configuration check" }; break;
                case "/apply-template": response = new { prompt = "test" }; break;
                case "/tokenize": response = new { tokens = Enumerable.Range(0, 100).ToArray() }; break;
                case "/v1/chat/completions":
                    RequestModel = JsonSerializer.Deserialize<JsonElement>(await request.Content!.ReadAsStringAsync(ct)).GetProperty("model").GetString();
                    response = new { choices = new[] { new { finish_reason = "stop", message = new { content = "{\"selected_id\":\"0\"}" } } }, usage = new { prompt_tokens = 100, completion_tokens = 7 } }; break;
                default: throw new InvalidOperationException("Unexpected adapter route");
            }
            return new(HttpStatusCode.OK) { Content = new StringContent(JsonSerializer.Serialize(response), Encoding.UTF8, "application/json") };
        }
    }
}
