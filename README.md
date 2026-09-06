# Clementine

Clementine is a modern music player and library organizer for Windows, Linux and macOS.

This repository is a maintained fork of Clementine with additional fixes, improvements, and platform integration work.

* [Latest Release](https://github.com/thellewitt/Clementine/releases/latest)
* [Latest Pre-Releases](https://github.com/thellewitt/Clementine/releases)
* Website: http://www.clementine-player.org/
* GitHub: https://github.com/thellewitt/Clementine

## Upgrading from an Earlier Version

**If you are upgrading from an earlier version of this fork, it is strongly recommended that you regenerate Clementine's configuration before running the new version.**

Recent changes affect configuration, database state, Extras, metadata handling, and file-type detection. Existing configuration and database files can contain information from older versions that prevents the new behavior from being applied correctly.

Rather than deleting the old configuration, **rename the entire Clementine configuration directory to `Clementine.bak`**. This preserves your previous configuration as a backup while allowing Clementine to create a completely fresh configuration and library database.

On Linux, the configuration directory is normally:

```text
~/.config/Clementine
```

For example:

```bash
mv ~/.config/Clementine ~/.config/Clementine.bak
```

Then start Clementine normally and allow it to generate a new configuration and rebuild the library database.

This is particularly important when upgrading to a version containing the updated **Extras** support and improved **MP4/M4A file-type classification**.

## Opening an Issue

### Ask for a New Feature

Please:

* Check whether the feature has already been implemented.
* Check whether another person has already opened an issue.
* If an issue already exists, add useful information to it rather than posting `+1`.

### Report a Bug

Please:

* Try the latest release or build first.
* Check whether the problem has already been reported.
* If an existing issue covers the problem, add useful details to it.
* Otherwise, open a new issue with a clear title and as much information as possible, including your operating system, Clementine version, and steps to reproduce the problem.
* Include relevant logs or debug output directly in the issue, preferably in a code block.

## Compiling from Source

Clone the repository:

```bash
git clone https://github.com/thellewitt/Clementine.git
cd Clementine
```

Create the build directory and configure the project:

```bash
cmake -S . -B build
```

Build Clementine:

```bash
cmake --build build -j$(nproc)
```

Install:

```bash
sudo cmake --install build
```

See the Wiki for additional build information and dependencies:

https://github.com/thellewitt/Clementine/wiki#compiling-and-installing-clementine
