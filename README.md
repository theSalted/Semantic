# semantic

An engine-independent C# framework workspace for future experiments.

The solution currently contains one intentionally empty class library,
`src/Semantic/Semantic.csproj`, with no application code or external package
references. Memory, goals, and attitude models remain open design questions.
Engine adapters can be explored later; this is not a Unity project.

## Tooling and target

Build with the .NET 10 SDK. `global.json` selects SDK 10.0.400 and permits
stable patches within that feature band.

The library targets .NET Standard 2.1 to keep its API surface usable by modern
.NET and compatible Mono-based engines, including Unity 2021.2 and newer.
The SDK used to build the library is separate from the runtime that eventually
hosts it. Engine integration and platform-specific behavior still need validation
when an adapter is added. See Microsoft's
[compatibility table](https://learn.microsoft.com/en-us/dotnet/standard/net-standard#net-standard-versions).

## Build

From this directory:

```sh
dotnet restore Semantic.sln
dotnet build Semantic.sln --configuration Release --no-restore
```

Restore may obtain Microsoft's .NET Standard reference pack if it is missing
from the SDK installation. No model runtime, ML library, or weights are needed.

Open this folder in VS Code or open `Semantic.sln` in a C# editor. Build outputs
and local tooling caches are excluded from Git.
