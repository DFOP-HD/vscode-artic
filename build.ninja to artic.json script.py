#!/usr/bin/env python3
"""
Parse a CMake-generated Ninja build file and extract artic compilation commands.
Generates an artic.json file containing the projects and their dependencies.
"""

import json
import re
import sys
from pathlib import Path
from typing import Dict, List, Set


def parse_ninja_build_file(ninja_file: str) -> Dict[str, List[str]]:
    """
    Parse a Ninja build file and extract artic compilation commands.
    
    Args:
        ninja_file: Path to the build.ninja file
        
    Returns:
        Dictionary mapping target names to lists of input files
    """
    projects = {}
    
    with open(ninja_file, 'r') as f:
        content = f.read()
    
    # Find all custom command sections that invoke artic
    # Pattern: find "build <target>: CUSTOM_COMMAND" followed by "COMMAND = ... /path/to/artic ..."
    
    # Split by "build " to find individual build statements
    build_blocks = re.split(r'\nbuild ', content)
    
    for block in build_blocks:
        lines = block.split('\n')
        
        if not lines:
            continue
            
        # First line should be the build statement
        build_line = lines[0]
        
        # Look for CUSTOM_COMMAND blocks that contain artic invocations
        if 'CUSTOM_COMMAND' not in build_line:
            continue
        
        # Extract the target name (e.g., "artic-sources/spmv_lrb-compute.ll")
        # Format: "target: CUSTOM_COMMAND dependencies..."
        match = re.match(r'^(\S+).*?CUSTOM_COMMAND', build_line)
        if not match:
            continue
            
        target_name = match.group(1)
        
        # Look for COMMAND line in the block
        command_line = None
        for line in lines[1:]:
            if line.strip().startswith('COMMAND = '):
                command_line = line.strip()
                break
        
        if not command_line:
            continue
        
        # Check if this command invokes artic
        if '/artic ' not in command_line and 'bin/artic ' not in command_line:
            continue
        
        # Extract all input files from the command
        # The artic command format is: artic [input files...] [options] -o output
        # We need to find all files passed to artic before the options start
        
        # Remove the "COMMAND = " prefix
        command = command_line[10:]  # len("COMMAND = ") == 10
        
        # Split the command by " && " to handle multi-command sequences
        artic_command = None
        for cmd_part in command.split(' && '):
            if '/artic ' in cmd_part or 'bin/artic ' in cmd_part:
                artic_command = cmd_part
                break
        
        if not artic_command:
            continue
        
        # Extract artic executable path
        artic_match = re.search(r'(/[^ ]*/artic|bin/artic)', artic_command)
        if not artic_match:
            continue
        
        artic_pos = artic_match.end()
        after_artic = artic_command[artic_pos:].strip()
        
        # Parse arguments: files come first, then options starting with "-"
        input_files = []
        parts = after_artic.split()
        
        for part in parts:
            # Stop when we hit options (starting with -)
            if part.startswith('-'):
                break
            
            # Skip if it's not a path/file
            if part.startswith('/') or '.impala' in part or '/' in part:
                input_files.append(part)
        
        if input_files:
            projects[target_name] = input_files
    
    return projects


def generate_artic_json(projects: Dict[str, List[str]], output_file: str = "artic.json"):
    """
    Generate artic.json configuration file.
    
    Args:
        projects: Dictionary mapping target names to input file lists
        output_file: Path to write the artic.json file
    """
    # Build the artic-config structure
    config = {
        "artic-config": "2.0",
        "projects": []
    }
    
    # Sort projects by name for consistent output
    for target_name in sorted(projects.keys()):
        project = {
            "name": target_name,
            "files": sorted(projects[target_name])
        }
        config["projects"].append(project)
    
    # Write to file
    with open(output_file, 'w') as f:
        json.dump(config, f, indent=4)
    
    print(f"Generated {output_file} with {len(config['projects'])} projects")


def main():
    """Main entry point."""
    if len(sys.argv) < 2:
        # Default to build/build.ninja if no argument provided
        ninja_file = "build/build.ninja"
        print(f"No input file specified, using default: {ninja_file}")
    else:
        ninja_file = sys.argv[1]
    
    if len(sys.argv) >= 3:
        output_file = sys.argv[2]
    else:
        output_file = "artic.json"
    
    # Check if file exists
    if not Path(ninja_file).exists():
        print(f"Error: {ninja_file} not found", file=sys.stderr)
        sys.exit(1)
    
    print(f"Parsing {ninja_file}...")
    projects = parse_ninja_build_file(ninja_file)
    
    if not projects:
        print("Warning: No artic projects found in the build file", file=sys.stderr)
    else:
        print(f"Found {len(projects)} artic projects:")
        for target_name, files in sorted(projects.items()):
            print(f"  {target_name}: {len(files)} files")
    
    generate_artic_json(projects, output_file)


if __name__ == "__main__":
    main()
