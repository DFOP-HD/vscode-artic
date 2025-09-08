# Workspace Configuration File

To define projects, place a `artic.json` file at the root of your vscode workspace.

Example:
```json
{
    "artic-config": "1.0",
    "projects": [
        {
            "name": "my-project",
            "folder": null, // optional: defaults to location of artic.json
            "dependencies": [
                "anydsl.runtime",
                "anydsl.artic-utils"
            ],
            "files": [
                "src/*.art",
                "!src/do-not-include.art", // exclude files with `!`
                "backend_amdgpu_pal.art"
            ]
        }
    ],
    // include projects from external atric.json files (may be recursive)
    "include": [
        // optional for global dependencies, allows you to define a fallback path
        {
            "projects": [ "anydsl.runtime" ],
            "path": "../anydsl/runtime/artic.json",
            "prefer-global": true
        },
        {
            "projects": [ "anydsl.artic-utils" ],
            "path": "../anydsl/artic-utils/artic.json",
            "prefer-global": true
        }
    ]
}
```

In the extension settings of this extension, you can define a global configuration file with the same schema


`artic-global.json`
```json
{
    "artic-config": "1.0",
    // use these defaults whenever you are editing at a file that does not belong to any project. always includes the active file
    "default-project": {
        "name": "<no project>",
        "folder": null,
        "dependencies": [
            "anydsl.runtime",
            "anydsl.artic-utils"
        ],
        "files": [
            "**/*.art",
            "**/*.impala"
        ]
    },
    "projects": [
        {
            "name": "anydsl.artic-utils",
            "folder": "~/repos/anydsl/artic-utils",
            "dependencies": [
                "anydsl.runtime"
            ],
            "files": [
                "artic/artic-utils.impala",
                "artic/vector-types.impala"
            ]
        }
    ],
    "include": [
        {
            "projects": [ "anydsl.runtime" ],
            "path": "~/anydsl/runtime/artic.json"
        },
    ]
}
```

# Implementation Details
- evaluating a project configuration yields a list of files with their corresponding project
- project dependencies can be recursive (will just combine both the included files)
- configuration includes can be recursive (if a include would introduce a loop, it should be ignored)
- should ignore files that are included multiple times
- should prompt you to create `artic-global.json` if it does not exist yet
- should give you errors when it cannot find a project (global or non-global)
- should allow you to define paths explicitly or as a pattern. Paths can be e.g. relative or absolute
- should give you errors when a explicit file does not exist
- should give you errors when a included artic.json file does not exist
- errors and warnings should be handled gracefully and displayed as a notification to the user
- ...




