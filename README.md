# VSFS Journaling Project

Hi! Welcome to this little exploration of file system reliability. This project is a demonstration of **Write-Ahead Logging (Journaling)** implemented on top of a custom "Very Simple File System" (VSFS).

The goal here isn't to build the next ext4 or NTFS, but to show how file systems keep your metadata consistent even if the power goes out.

## What's This All About?

File systems are fragile. If a crash happens while you're writing a file, you might end up with a corrupted disk. **Journaling** fixes this by writing down what we *plan* to do (in a "journal") before we actually overwrite the important structures on the disk.

This project simulates that process with three main tools:

1.  **`mkfs.c` (The Creator)**: This tool initializes a fresh disk image (`vsfs.img`). Think of it as formatting your drive. It sets up the superblock, clears the bitmaps, and prepares the root directory.
    
2.  **`journal.c` (The Worker)**: This is where the magic happens. It handles file operations in two steps:
    *   `create`: When you ask to create a file, it doesn't touch the main file system yet. Instead, it writes a transaction to the **journal** area.
    *   `install`: This is the "checkpoint" step. It scans the journal for completed transactions and replays them onto the main file system, making the changes permanent and safe.

3.  **`validator.c` (The Inspector)**: This is our safety net (like `fsck`). It scans the disk image to make sure everything looks right—checking that inodes match their bitmaps, directory structures are valid, and no blocks are missing.

## Getting Started

First, let's compile the tools. You'll need `gcc` installed.

```bash
gcc -o mkfs mkfs.c
gcc -o journal journal.c
gcc -o validator validator.c
```

## A Quick Walkthrough

Ready to see it in action? Here is a simple workflow:

**1. Format the Disk**  
Start with a clean slate. This creates a brand new `vsfs.img` file.
```bash
./mkfs
```

**2. Make a Change (Safely)**  
Let's create a file called `hello.txt`. Notice that if you checked the file system right after this, the file wouldn't appear in the main directory yet—it's safely tucked away in the journal.
```bash
./journal create hello.txt
```

**3. Make it Permanent**  
Now, we "commit" the journal. This takes those pending changes and applies them to the actual file system structures.
```bash
./journal install
```

**4. Check Your Work**  
Finally, run the validator to prove that our file system is healthy and consistent.
```bash
./validator
```

---
*Feel free to explore the code! `journal.c` is particularly interesting if you want to see how the transaction structures (`data_record`, `commit_record`) are laid out.*
