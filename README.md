# resfs
Recovery-First File System (ResFS) is a file system in which every physically existing file is reconstructable from raw disk bytes even if all metadata, journals, and superblocks are completely destroyed. Every segment has its own metadata. This FS resolves the key problem of traditional recovery utilities of file fragmentation.
