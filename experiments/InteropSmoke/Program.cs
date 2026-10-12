using System;
using Semantic;

internal static class Program
{
    private static int Main()
    {
        string? info = Native.BackendInfo;
        if (string.IsNullOrWhiteSpace(info))
        {
            Console.Error.WriteLine("llama.cpp did not report any backend information.");
            return 1;
        }

        Console.WriteLine($"Semantic C# -> native -> llama.cpp smoke passed: {info}");
        return 0;
    }
}
