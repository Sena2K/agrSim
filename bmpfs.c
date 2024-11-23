#define FUSE_USE_VERSION 31
#define BMPFS_OPT(t, p) {t, offsetof(struct bmpfs_config, p), 1}

#include <errno.h>
#include <fcntl.h>
#include <fuse3/fuse.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdarg.h>
#include <time.h>
#include <limits.h>

// Empacotamento para evitar padding na estrutura
#pragma pack(push, 1)
typedef struct {
    char filename[256];
    uint64_t size;
    uint64_t created;
    uint64_t modified;
    uint64_t accessed;
    uint32_t first_block;
    uint32_t num_blocks;
    uint32_t mode;
    uint32_t uid;
    uint32_t gid;
    uint8_t is_dir; // 1 para diretório, 0 para arquivo
} FileMetadata;
#pragma pack(pop)

// Verificação estática do tamanho da estrutura
_Static_assert(sizeof(FileMetadata) == 309, "FileMetadata size must be 309 bytes");

// BMP file header structure (14 bytes)
#pragma pack(push, 1)
typedef struct {
    uint16_t signature;  // "BM"
    uint32_t filesize;   // Size of the BMP file in bytes
    uint16_t reserved1;  // Reserved
    uint16_t reserved2;  // Reserved
    uint32_t dataOffset; // Offset to pixel data
} BMPHeader;
#pragma pack(pop)

// BMP info header structure (40 bytes)
#pragma pack(push, 1)
typedef struct {
    uint32_t headerSize;      // Size of this header
    int32_t width;            // Image width
    int32_t height;           // Image height
    uint16_t planes;          // Number of color planes
    uint16_t bitsPerPixel;    // Bits per pixel
    uint32_t compression;     // Compression type
    uint32_t imageSize;       // Image size in bytes
    int32_t xPixelsPerM;      // Pixels per meter in x
    int32_t yPixelsPerM;      // Pixels per meter in y
    uint32_t colorsUsed;      // Number of colors used
    uint32_t colorsImportant; // Important colors
} BMPInfoHeader;
#pragma pack(pop)

// Debug logging function
static void debug_log(const char *format, ...) {
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
}

// Configuration structure
struct bmpfs_config {
    char *image_path;
};

static struct bmpfs_config config; // Global config instance

static struct fuse_opt bmpfs_opts[] = {
    BMPFS_OPT("image=%s", image_path),
    FUSE_OPT_END
};

// File system state
typedef struct {
    FILE *bmp_file;
    BMPHeader header;
    BMPInfoHeader info_header;
    size_t data_size;    // Total available data size (bytes)
    size_t block_size;   // Size of each block (in bytes)
    uint8_t *bitmap;     // Bitmap for block allocation
    FileMetadata *files; // Array of file metadata
    size_t max_files;    // Maximum number of files
    char *image_path;    // Image path
} bmp_fs_state;

static bmp_fs_state fs_state;

// Calculate the size of the metadata (bitmap + file metadata)
static size_t calculate_metadata_size(bmp_fs_state *state) {
    size_t total_blocks = state->data_size / state->block_size;
    size_t bitmap_size = total_blocks; // 1 byte per block
    size_t file_metadata_size = state->max_files * sizeof(FileMetadata);
    return bitmap_size + file_metadata_size;
}

// Read metadata from the BMP file
static int read_metadata(bmp_fs_state *state) {
    size_t metadata_size = calculate_metadata_size(state);
    char *buffer = malloc(metadata_size);
    if (!buffer) {
        debug_log("Failed to allocate buffer for metadata\n");
        return -ENOMEM;
    }

    // Seek to the start of pixel data
    if (fseek(state->bmp_file, state->header.dataOffset, SEEK_SET) != 0) {
        debug_log("Failed to seek to metadata area\n");
        free(buffer);
        return -EIO;
    }

    // Read metadata
    size_t read_bytes = fread(buffer, 1, metadata_size, state->bmp_file);
    if (read_bytes != metadata_size) {
        debug_log("Failed to read metadata area: read %zu bytes, expected %zu bytes\n", read_bytes, metadata_size);
        free(buffer);
        return -EIO;
    }

    // Assign bitmap and file metadata
    size_t bitmap_size = state->data_size / state->block_size;
    memcpy(state->bitmap, buffer, bitmap_size);
    memcpy(state->files, buffer + bitmap_size, state->max_files * sizeof(FileMetadata));

    free(buffer);
    return 0;
}

// Write metadata to the BMP file
static int write_metadata(bmp_fs_state *state) {
    size_t metadata_size = calculate_metadata_size(state);
    char *buffer = malloc(metadata_size);
    if (!buffer) {
        debug_log("Failed to allocate buffer for metadata\n");
        return -ENOMEM;
    }

    // Combine bitmap and file metadata into buffer
    size_t bitmap_size = state->data_size / state->block_size;
    memcpy(buffer, state->bitmap, bitmap_size);
    memcpy(buffer + bitmap_size, state->files, state->max_files * sizeof(FileMetadata));

    // Seek to the start of pixel data
    if (fseek(state->bmp_file, state->header.dataOffset, SEEK_SET) != 0) {
        debug_log("Failed to seek to metadata area\n");
        free(buffer);
        return -EIO;
    }

    // Write metadata
    size_t written = fwrite(buffer, 1, metadata_size, state->bmp_file);
    if (written != metadata_size) {
        debug_log("Failed to write metadata area: wrote %zu bytes, expected %zu bytes\n", written, metadata_size);
        free(buffer);
        return -EIO;
    }

    // Flush changes to disk
    if (fflush(state->bmp_file) != 0) {
        debug_log("Failed to flush metadata to disk\n");
        free(buffer);
        return -EIO;
    }

    free(buffer);
    return 0;
}

// Create a new BMP file with proper error handling
static int create_bmp_file(const char *filename, size_t width, size_t height) {
    debug_log("Attempting to create/open BMP file: %s\n", filename);

    // Check directory permissions first
    char *dir_path = strdup(filename);
    if (!dir_path) {
        debug_log("Failed to duplicate filename for directory path\n");
        return -ENOMEM;
    }
    char *last_slash = strrchr(dir_path, '/');
    if (last_slash) {
        *last_slash = '\0';
    } else {
        dir_path[0] = '.';
        dir_path[1] = '\0';
    }

    // Check if we can write to the directory
    if (access(dir_path, W_OK) != 0) {
        debug_log("Directory %s is not writable (errno: %d - %s)\n", dir_path, errno, strerror(errno));
        free(dir_path);
        return -errno;
    }
    free(dir_path);

    // Check if file already exists
    if (access(filename, F_OK) == 0) {
        // File exists, check if we can write to it
        if (access(filename, W_OK) != 0) {
            debug_log("Existing file %s is not writable (errno: %d - %s)\n", filename, errno, strerror(errno));
            return -errno;
        }
        debug_log("File already exists and is writable\n");
        return 0;
    }

    // Try to create the file with explicit permissions
    int fd = open(filename, O_WRONLY | O_CREAT, 0644);
    if (fd == -1) {
        debug_log("Failed to create file with open() (errno: %d - %s)\n", errno, strerror(errno));
        return -errno;
    }
    FILE *fp = fdopen(fd, "wb");
    if (!fp) {
        debug_log("Failed to create FILE* from fd (errno: %d - %s)\n", errno, strerror(errno));
        close(fd);
        return -errno;
    }

    // Calculate row size with padding (each row is padded to a multiple of 4 bytes)
    size_t row_size = (width * 3 + 3) & ~3;
    size_t pixel_data_size = row_size * height;

    size_t file_size;
    if (__builtin_add_overflow(sizeof(BMPHeader) + sizeof(BMPInfoHeader), pixel_data_size, &file_size)) {
        debug_log("Overflow detected when calculating file size\n");
        fclose(fp);
        return -EOVERFLOW;
    }

    // Create and write headers
    BMPHeader header = {
        .signature = 0x4D42, // "BM" in little endian
        .filesize = file_size,
        .reserved1 = 0,
        .reserved2 = 0,
        .dataOffset = sizeof(BMPHeader) + sizeof(BMPInfoHeader)
    };

    BMPInfoHeader info_header = {
        .headerSize = sizeof(BMPInfoHeader),
        .width = (int32_t)width,
        .height = (int32_t)height,
        .planes = 1,
        .bitsPerPixel = 24,
        .compression = 0,
        .imageSize = (uint32_t)pixel_data_size,
        .xPixelsPerM = 2835,
        .yPixelsPerM = 2835,
        .colorsUsed = 0,
        .colorsImportant = 0
    };

    if (fwrite(&header, sizeof(BMPHeader), 1, fp) != 1 ||
        fwrite(&info_header, sizeof(BMPInfoHeader), 1, fp) != 1) {
        debug_log("Failed to write BMP headers (errno: %d - %s)\n", errno, strerror(errno));
        fclose(fp);
        return -errno;
    }

    // Initialize pixel data to zero
    unsigned char *pixel_data = calloc(1, pixel_data_size);
    if (!pixel_data) {
        debug_log("Failed to allocate pixel data memory\n");
        fclose(fp);
        return -ENOMEM;
    }

    size_t written_pixels = fwrite(pixel_data, 1, pixel_data_size, fp);
    free(pixel_data);

    if (written_pixels != pixel_data_size) {
        debug_log("Failed to write pixel data (errno: %d - %s)\n", errno, strerror(errno));
        fclose(fp);
        return -errno;
    }

    // Initialize bitmap and files structures
    size_t total_blocks = pixel_data_size / 512;
    size_t bitmap_size = total_blocks; // 1 byte per block
    size_t max_files = 1000;
    size_t file_metadata_size = max_files * sizeof(FileMetadata);
    size_t metadata_size = bitmap_size + file_metadata_size;

    char *initial_metadata = calloc(1, metadata_size);
    if (!initial_metadata) {
        debug_log("Failed to allocate initial metadata\n");
        fclose(fp);
        return -ENOMEM;
    }

    // Seek to the start of pixel data
    if (fseek(fp, header.dataOffset, SEEK_SET) != 0) {
        debug_log("Failed to seek to metadata area during creation\n");
        free(initial_metadata);
        fclose(fp);
        return -EIO;
    }

    // Write initial metadata (all zeros)
    size_t metadata_written = fwrite(initial_metadata, 1, metadata_size, fp);
    free(initial_metadata);

    if (metadata_written != metadata_size) {
        debug_log("Failed to write initial metadata (errno: %d - %s)\n", errno, strerror(errno));
        fclose(fp);
        return -EIO;
    }

    // Flush changes to disk
    if (fflush(fp) != 0) {
        debug_log("Failed to flush BMP file after creation (errno: %d - %s)\n", errno, strerror(errno));
        fclose(fp);
        return -EIO;
    }

    debug_log("Successfully created BMP file with initial metadata\n");
    fclose(fp);
    return 0;
}

// Improved path validation
static int validate_path(const char *path) {
    if (!path || strlen(path) >= 256) {
        return -ENAMETOOLONG;
    }

    // Skip leading slash for validation
    if (path[0] == '/') {
        path++;
    }

    // Check for invalid characters (no slashes allowed for este exemplo)
    if (strchr(path, '/')) {
        return -EINVAL;
    }

    return 0;
}

// Convert file path to metadata index with validation
static int path_to_metadata_index(const char *path) {
    int validation = validate_path(path);
    if (validation < 0) {
        return validation;
    }

    // Skip leading '/'
    if (path[0] == '/') {
        path++;
    }

    for (size_t i = 0; i < fs_state.max_files; i++) {
        if (fs_state.files[i].filename[0] != '\0' &&
            strcmp(fs_state.files[i].filename, path) == 0) {
            return i;
        }
    }
    return -ENOENT;
}

// Find free blocks with improved efficiency
static uint32_t find_free_blocks(size_t num_blocks) {
    if (num_blocks == 0) {
        return 0;
    }

    size_t total_blocks = fs_state.data_size / fs_state.block_size;
    size_t consecutive = 0;
    uint32_t start_block = 0;

    for (size_t i = 0; i < total_blocks; i++) {
        if (fs_state.bitmap[i] == 0) {
            if (consecutive == 0) {
                start_block = i;
            }
            consecutive++;
            if (consecutive >= num_blocks) {
                return start_block;
            }
        } else {
            consecutive = 0;
        }
    }
    return UINT32_MAX;
}

// Improved block operations with error checking
static int read_blocks(uint32_t start_block, size_t num_blocks, char *buffer) {
    if (!buffer || !fs_state.bmp_file) {
        return -EINVAL;
    }

    size_t metadata_size = calculate_metadata_size(&fs_state);
    size_t offset = fs_state.header.dataOffset + metadata_size +
                    (start_block * fs_state.block_size);
    if (fseek(fs_state.bmp_file, offset, SEEK_SET) != 0) {
        debug_log("Failed to seek to blocks for reading (errno: %d - %s)\n", errno, strerror(errno));
        return -EIO;
    }

    size_t bytes_read = fread(buffer, 1, fs_state.block_size * num_blocks, fs_state.bmp_file);
    if (bytes_read != fs_state.block_size * num_blocks) {
        debug_log("Failed to read blocks: read %zu bytes, expected %zu bytes\n", bytes_read, fs_state.block_size * num_blocks);
        return -EIO;
    }

    return 0;
}

static int write_blocks(uint32_t start_block, size_t num_blocks, const char *buffer) {
    if (!buffer || !fs_state.bmp_file) {
        return -EINVAL;
    }

    size_t metadata_size = calculate_metadata_size(&fs_state);
    size_t offset = fs_state.header.dataOffset + metadata_size +
                    (start_block * fs_state.block_size);
    if (fseek(fs_state.bmp_file, offset, SEEK_SET) != 0) {
        debug_log("Failed to seek to blocks for writing (errno: %d - %s)\n", errno, strerror(errno));
        return -EIO;
    }

    size_t bytes_written = fwrite(buffer, 1, fs_state.block_size * num_blocks, fs_state.bmp_file);
    if (bytes_written != fs_state.block_size * num_blocks) {
        debug_log("Failed to write blocks: wrote %zu bytes, expected %zu bytes\n", bytes_written, fs_state.block_size * num_blocks);
        return -EIO;
    }

    if (fflush(fs_state.bmp_file) != 0) {
        debug_log("Failed to flush blocks to disk (errno: %d - %s)\n", errno, strerror(errno));
        return -EIO;
    }

    return 0;
}

// Função para criar diretórios
static int bmpfs_mkdir(const char *path, mode_t mode) {
    debug_log("Creating directory: %s\n", path);

    int validation = validate_path(path);
    if (validation < 0) {
        debug_log("Path validation failed: %d\n", validation);
        return validation;
    }

    // Verificar se o diretório já existe
    if (path_to_metadata_index(path) >= 0) {
        debug_log("Directory already exists\n");
        return -EEXIST;
    }

    // Encontrar um slot de metadados vazio
    int idx = -1;
    for (size_t i = 0; i < fs_state.max_files; i++) {
        if (fs_state.files[i].filename[0] == '\0') {
            idx = i;
            break;
        }
    }

    if (idx < 0) {
        debug_log("No free metadata slots\n");
        return -ENOMEM;
    }

    // Inicializar metadados
    FileMetadata *meta = &fs_state.files[idx];
    const char *dirname = path;
    if (path[0] == '/') {
        dirname++;
    }

    strncpy(meta->filename, dirname, sizeof(meta->filename) - 1);
    meta->filename[sizeof(meta->filename) - 1] = '\0';
    meta->size = 0;
    meta->created = time(NULL);
    meta->modified = meta->created;
    meta->accessed = meta->created;
    meta->first_block = UINT32_MAX; // Nenhum bloco alocado ainda
    meta->num_blocks = 0;
    meta->mode = S_IFDIR | (mode & 0777); // Definir como diretório
    meta->uid = getuid();
    meta->gid = getgid();
    meta->is_dir = 1;

    debug_log("Directory created successfully: %s (idx: %d)\n", path, idx);

    // Escrever metadados atualizados no arquivo BMP
    if (write_metadata(&fs_state) < 0) {
        debug_log("Failed to write metadata after directory creation\n");
        return -EIO;
    }

    return 0;
}

// FUSE operations com suporte a diretórios
static int bmpfs_getattr(const char *path, struct stat *stbuf,
                         struct fuse_file_info *fi) {
    memset(stbuf, 0, sizeof(struct stat));

    if (strcmp(path, "/") == 0) {
        stbuf->st_mode = S_IFDIR | 0755;
        stbuf->st_nlink = 2;
        stbuf->st_uid = getuid();
        stbuf->st_gid = getgid();
        stbuf->st_atime = time(NULL);
        stbuf->st_mtime = stbuf->st_atime;
        stbuf->st_ctime = stbuf->st_atime;
        return 0;
    }

    int idx = path_to_metadata_index(path);
    if (idx < 0) {
        return idx;
    }

    FileMetadata *meta = &fs_state.files[idx];
    stbuf->st_mode = meta->mode;
    stbuf->st_nlink = meta->is_dir ? 2 : 1; // Diretórios têm pelo menos 2 links
    stbuf->st_size = meta->size;
    stbuf->st_uid = meta->uid;
    stbuf->st_gid = meta->gid;
    stbuf->st_atime = meta->accessed;
    stbuf->st_mtime = meta->modified;
    stbuf->st_ctime = meta->created;
    stbuf->st_blocks = (meta->size + 511) / 512; // Tamanho padrão do bloco
    stbuf->st_blksize = fs_state.block_size;

    return 0;
}

static int bmpfs_create(const char *path, mode_t mode,
                        struct fuse_file_info *fi) {
    debug_log("Creating file: %s\n", path);

    int validation = validate_path(path);
    if (validation < 0) {
        debug_log("Path validation failed: %d\n", validation);
        return validation;
    }

    // Check if the file already exists
    if (path_to_metadata_index(path) >= 0) {
        debug_log("File already exists\n");
        return -EEXIST;
    }

    // Find an empty metadata slot
    int idx = -1;
    for (size_t i = 0; i < fs_state.max_files; i++) {
        if (fs_state.files[i].filename[0] == '\0') {
            idx = i;
            break;
        }
    }

    if (idx < 0) {
        debug_log("No free metadata slots\n");
        return -ENOMEM;
    }

    // Initialize metadata
    FileMetadata *meta = &fs_state.files[idx];
    const char *filename = path;
    if (path[0] == '/') {
        filename++;
    }

    strncpy(meta->filename, filename, sizeof(meta->filename) - 1);
    meta->filename[sizeof(meta->filename) - 1] = '\0';
    meta->size = 0;
    meta->created = time(NULL);
    meta->modified = meta->created;
    meta->accessed = meta->created;
    meta->first_block = UINT32_MAX; // No blocks allocated yet
    meta->num_blocks = 0;
    meta->mode = S_IFREG | (mode & 0777);
    meta->uid = getuid();
    meta->gid = getgid();
    meta->is_dir = 0;

    debug_log("File created successfully: %s (idx: %d)\n", path, idx);

    // Write updated metadata to the BMP file
    if (write_metadata(&fs_state) < 0) {
        debug_log("Failed to write metadata after file creation\n");
        return -EIO;
    }

    return 0;
}

static void *bmpfs_init(struct fuse_conn_info *conn, struct fuse_config *cfg) {
    debug_log("Initializing filesystem...\n");

    cfg->kernel_cache = 1;
    cfg->entry_timeout = 60.0;
    cfg->attr_timeout = 60.0;

    if (!fs_state.image_path) {
        debug_log("No image path provided\n");
        return NULL;
    }

    debug_log("Checking file: %s\n", fs_state.image_path);

    fs_state.bmp_file = fopen(fs_state.image_path, "r+b");
    if (!fs_state.bmp_file) {
        debug_log("Could not open existing file (errno: %d - %s)\n", errno, strerror(errno));

        int create_result = create_bmp_file(fs_state.image_path, 2048, 2048);
        if (create_result < 0) {
            debug_log("Failed to create BMP file: %d (errno: %d - %s)\n",
                      create_result, errno, strerror(errno));
            return NULL;
        }

        fs_state.bmp_file = fopen(fs_state.image_path, "r+b");
        if (!fs_state.bmp_file) {
            debug_log("Failed to open created BMP file (errno: %d - %s)\n", errno, strerror(errno));
            return NULL;
        }
    }

    // Verificar permissões
    int fd = fileno(fs_state.bmp_file);
    if (fd == -1) {
        debug_log("Failed to get file descriptor\n");
        fclose(fs_state.bmp_file);
        return NULL;
    }

    struct stat st;
    if (fstat(fd, &st) == -1) {
        debug_log("Failed to get file stats\n");
        fclose(fs_state.bmp_file);
        return NULL;
    }

    if ((st.st_mode & S_IRUSR) == 0 || (st.st_mode & S_IWUSR) == 0) {
        debug_log("Insufficient permissions for BMP file\n");
        fclose(fs_state.bmp_file);
        return NULL;
    }

    // Read BMP headers
    BMPHeader header;
    BMPInfoHeader info_header;

    if (fread(&header, sizeof(BMPHeader), 1, fs_state.bmp_file) != 1) {
        debug_log("Failed to read BMP header\n");
        fclose(fs_state.bmp_file);
        return NULL;
    }

    if (header.signature != 0x4D42) { // "BM" in little endian
        debug_log("Invalid BMP signature\n");
        fclose(fs_state.bmp_file);
        return NULL;
    }

    if (fread(&info_header, sizeof(BMPInfoHeader), 1, fs_state.bmp_file) != 1) {
        debug_log("Failed to read BMP info header\n");
        fclose(fs_state.bmp_file);
        return NULL;
    }

    fs_state.header = header;
    fs_state.info_header = info_header;

    // Calculate row size with padding
    size_t row_size = (info_header.width * 3 + 3) & ~3;
    fs_state.data_size = row_size * info_header.height;
    fs_state.block_size = 512;
    fs_state.max_files = 1000;

    debug_log("Filesystem parameters:\n");
    debug_log("  Data size: %zu bytes\n", fs_state.data_size);
    debug_log("  Block size: %zu bytes\n", fs_state.block_size);
    debug_log("  Max files: %zu\n", fs_state.max_files);

    size_t bitmap_size = fs_state.data_size / fs_state.block_size;
    fs_state.bitmap = calloc(bitmap_size, sizeof(uint8_t));
    if (!fs_state.bitmap) {
        debug_log("Failed to allocate bitmap\n");
        fclose(fs_state.bmp_file);
        return NULL;
    }

    fs_state.files = calloc(fs_state.max_files, sizeof(FileMetadata));
    if (!fs_state.files) {
        debug_log("Failed to allocate file metadata array\n");
        free(fs_state.bitmap);
        fclose(fs_state.bmp_file);
        return NULL;
    }

    // Read existing metadata
    if (read_metadata(&fs_state) < 0) {
        debug_log("Failed to read metadata\n");
        free(fs_state.bitmap);
        free(fs_state.files);
        fclose(fs_state.bmp_file);
        return NULL;
    }

    debug_log("Filesystem initialized successfully\n");
    return &fs_state;
}

static void bmpfs_destroy(void *private_data) {
    // Write metadata to the BMP file
    if (write_metadata(&fs_state) < 0) {
        debug_log("Failed to write metadata on destroy\n");
    }

    // Close the BMP file if it's open
    if (fs_state.bmp_file) {
        fclose(fs_state.bmp_file);
        fs_state.bmp_file = NULL;
    }

    // Free allocated memory for bitmap, files, and image path
    free(fs_state.bitmap);
    fs_state.bitmap = NULL;
    free(fs_state.files);
    fs_state.files = NULL;
    free(fs_state.image_path);
    fs_state.image_path = NULL;
}

static int bmpfs_unlink(const char *path) {
    int idx = path_to_metadata_index(path);
    if (idx < 0) {
        return idx;
    }

    FileMetadata *meta = &fs_state.files[idx];

    // Verificar se é um diretório
    if (meta->is_dir) {
        debug_log("Cannot unlink a directory: %s\n", path);
        return -EISDIR;
    }

    // Liberar blocos no bitmap
    for (uint32_t i = 0; i < meta->num_blocks; i++) {
        fs_state.bitmap[meta->first_block + i] = 0;
    }

    // Clear metadata
    memset(meta, 0, sizeof(FileMetadata));

    // Write updated metadata to the BMP file
    if (write_metadata(&fs_state) < 0) {
        debug_log("Failed to write metadata after file deletion\n");
        return -EIO;
    }

    debug_log("File deleted successfully: %s (idx: %d)\n", path, idx);
    return 0;
}

static int bmpfs_read(const char *path, char *buf, size_t size, off_t offset,
                      struct fuse_file_info *fi) {
    if (!buf) {
        return -EINVAL;
    }

    int idx = path_to_metadata_index(path);
    if (idx < 0) {
        return idx;
    }

    FileMetadata *meta = &fs_state.files[idx];

    // Não é possível ler diretórios
    if (meta->is_dir) {
        return -EISDIR;
    }

    // Update access time
    meta->accessed = time(NULL);

    if (offset >= meta->size) {
        return 0;
    }

    // Adjust size if reading beyond end of file
    if (offset + size > meta->size) {
        size = meta->size - offset;
    }

    // Calculate block position
    uint32_t start_block = meta->first_block + (offset / fs_state.block_size);
    size_t block_offset = offset % fs_state.block_size;
    size_t blocks_to_read =
        (size + block_offset + fs_state.block_size - 1) / fs_state.block_size;

    // Allocate temporary buffer for aligned reading
    char *temp_buf = malloc(blocks_to_read * fs_state.block_size);
    if (!temp_buf) {
        return -ENOMEM;
    }

    int read_result = read_blocks(start_block, blocks_to_read, temp_buf);
    if (read_result < 0) {
        free(temp_buf);
        return read_result;
    }

    memcpy(buf, temp_buf + block_offset, size);
    free(temp_buf);

    debug_log("Read %zu bytes from file: %s (offset: %ld)\n", size, path, offset);
    return size;
}

static int bmpfs_write(const char *path, const char *buf, size_t size,
                       off_t offset, struct fuse_file_info *fi) {
    debug_log("Writing to file: %s (size: %zu, offset: %ld)\n", path, size, offset);

    if (!buf) {
        debug_log("Invalid buffer\n");
        return -EINVAL;
    }

    int idx = path_to_metadata_index(path);
    if (idx < 0) {
        debug_log("File not found: %d\n", idx);
        return idx;
    }

    FileMetadata *meta = &fs_state.files[idx];
    
    // Não é possível escrever em diretórios
    if (meta->is_dir) {
        debug_log("Cannot write to a directory: %s\n", path);
        return -EISDIR;
    }

    size_t new_size = offset + size;

    // Check for overflow
    if (new_size < offset) {
        debug_log("File size overflow\n");
        return -EFBIG;
    }

    // Calculate required blocks
    size_t new_blocks =
        (new_size + fs_state.block_size - 1) / fs_state.block_size;
    debug_log("Required blocks: %zu (current: %u)\n", new_blocks, meta->num_blocks);

    // If more blocks are needed
    if (new_blocks > meta->num_blocks) {
        uint32_t new_start = find_free_blocks(new_blocks);
        if (new_start == UINT32_MAX) {
            debug_log("No free blocks available\n");
            return -ENOSPC;
        }

        debug_log("Allocated new blocks starting at: %u\n", new_start);

        // If blocks already allocated, copy existing data
        if (meta->num_blocks > 0) {
            char *temp_buf = malloc(meta->num_blocks * fs_state.block_size);
            if (!temp_buf) {
                debug_log("Failed to allocate temporary buffer\n");
                return -ENOMEM;
            }

            int read_result =
                read_blocks(meta->first_block, meta->num_blocks, temp_buf);
            if (read_result < 0) {
                debug_log("Failed to read existing blocks: %d\n", read_result);
                free(temp_buf);
                return read_result;
            }

            int write_result = write_blocks(new_start, meta->num_blocks, temp_buf);
            free(temp_buf);

            if (write_result < 0) {
                debug_log("Failed to write to new blocks: %d\n", write_result);
                return write_result;
            }

            // Free old blocks in bitmap
            for (uint32_t i = 0; i < meta->num_blocks; i++) {
                fs_state.bitmap[meta->first_block + i] = 0;
            }
        }

        // Update metadata and mark new blocks as used
        meta->first_block = new_start;
        for (uint32_t i = 0; i < new_blocks; i++) {
            fs_state.bitmap[new_start + i] = 1;
        }
        meta->num_blocks = new_blocks;
    }

    // Perform the actual write
    uint32_t start_block = meta->first_block + (offset / fs_state.block_size);
    size_t block_offset = offset % fs_state.block_size;
    size_t blocks_to_write =
        (size + block_offset + fs_state.block_size - 1) / fs_state.block_size;

    char *temp_buf = malloc(blocks_to_write * fs_state.block_size);
    if (!temp_buf) {
        debug_log("Failed to allocate write buffer\n");
        return -ENOMEM;
    }

    // If not writing a full block, read existing data first
    if (block_offset > 0 || (size % fs_state.block_size) != 0) {
        int read_result = read_blocks(start_block, blocks_to_write, temp_buf);
        if (read_result < 0) {
            debug_log("Failed to read blocks for partial write: %d\n", read_result);
            free(temp_buf);
            return read_result;
        }
    } else {
        // Fill buffer with zeros if writing full blocks
        memset(temp_buf, 0, blocks_to_write * fs_state.block_size);
    }

    // Copy new data into the buffer
    memcpy(temp_buf + block_offset, buf, size);

    // Write the data
    int write_result = write_blocks(start_block, blocks_to_write, temp_buf);
    free(temp_buf);

    if (write_result < 0) {
        debug_log("Failed to write blocks: %d\n", write_result);
        return write_result;
    }

    // Update file size if necessary
    if (new_size > meta->size) {
        meta->size = new_size;
    }
    meta->modified = time(NULL);

    debug_log("Write successful: %zu bytes written\n", size);

    // Write updated metadata to the BMP file
    if (write_metadata(&fs_state) < 0) {
        debug_log("Failed to write metadata after file write\n");
        return -EIO;
    }

    return size;
}

static int bmpfs_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                         off_t offset, struct fuse_file_info *fi,
                         enum fuse_readdir_flags flags) {
    if (strcmp(path, "/") != 0) {
        return -ENOENT;
    }

    // Add default entries
    if (filler(buf, ".", NULL, 0, 0) || filler(buf, "..", NULL, 0, 0)) {
        return -ENOMEM;
    }

    // Add all non-empty files and directories
    for (size_t i = 0; i < fs_state.max_files; i++) {
        if (fs_state.files[i].filename[0] != '\0') {
            struct stat st;
            memset(&st, 0, sizeof(struct stat));
            st.st_mode = fs_state.files[i].mode;
            st.st_nlink = fs_state.files[i].is_dir ? 2 : 1;
            st.st_size = fs_state.files[i].size;
            st.st_uid = fs_state.files[i].uid;
            st.st_gid = fs_state.files[i].gid;
            st.st_atime = fs_state.files[i].accessed;
            st.st_mtime = fs_state.files[i].modified;
            st.st_ctime = fs_state.files[i].created;
            st.st_blocks = (fs_state.files[i].size + 511) / 512;
            st.st_blksize = fs_state.block_size;

            if (fs_state.files[i].is_dir) {
                st.st_mode |= S_IFDIR;
            } else {
                st.st_mode |= S_IFREG;
            }

            if (filler(buf, fs_state.files[i].filename, &st, 0, 0)) {
                return -ENOMEM;
            }
        }
    }

    return 0;
}

static int bmpfs_truncate(const char *path, off_t size,
                          struct fuse_file_info *fi) {
    if (size < 0) {
        return -EINVAL;
    }

    int idx = path_to_metadata_index(path);
    if (idx < 0) {
        return idx;
    }

    FileMetadata *meta = &fs_state.files[idx];

    // Não é possível truncar diretórios
    if (meta->is_dir) {
        debug_log("Cannot truncate a directory: %s\n", path);
        return -EISDIR;
    }

    // Calculate new number of blocks
    size_t new_blocks = (size + fs_state.block_size - 1) / fs_state.block_size;

    // If truncating to zero, free all blocks
    if (size == 0) {
        for (uint32_t i = 0; i < meta->num_blocks; i++) {
            fs_state.bitmap[meta->first_block + i] = 0;
        }
        meta->first_block = UINT32_MAX;
        meta->num_blocks = 0;
        meta->size = 0;
        meta->modified = time(NULL);
    }
    // If reducing
    else if (new_blocks < meta->num_blocks) {
        for (uint32_t i = new_blocks; i < meta->num_blocks; i++) {
            fs_state.bitmap[meta->first_block + i] = 0;
        }
        meta->num_blocks = new_blocks;
        meta->size = size;
        meta->modified = time(NULL);
    }
    // If increasing
    else if (new_blocks > meta->num_blocks) {
        uint32_t new_start = find_free_blocks(new_blocks);
        if (new_start == UINT32_MAX) {
            return -ENOSPC;
        }

        // Copy existing data if any
        if (meta->num_blocks > 0) {
            char *temp_buf = malloc(meta->num_blocks * fs_state.block_size);
            if (!temp_buf) {
                return -ENOMEM;
            }

            int read_result =
                read_blocks(meta->first_block, meta->num_blocks, temp_buf);
            if (read_result < 0) {
                debug_log("Failed to read existing blocks during truncate: %d\n",
                          read_result);
                free(temp_buf);
                return read_result;
            }

            int write_result = write_blocks(new_start, meta->num_blocks, temp_buf);
            free(temp_buf);

            if (write_result < 0) {
                debug_log("Failed to write to new blocks during truncate: %d\n",
                          write_result);
                return write_result;
            }

            // Free old blocks in bitmap
            for (uint32_t i = 0; i < meta->num_blocks; i++) {
                fs_state.bitmap[meta->first_block + i] = 0;
            }
        }

        // Mark new blocks as used
        for (uint32_t i = 0; i < new_blocks; i++) {
            fs_state.bitmap[new_start + i] = 1;
        }

        meta->first_block = new_start;
        meta->num_blocks = new_blocks;
        meta->size = size;
        meta->modified = time(NULL);
    }

    // Write updated metadata to the BMP file
    if (write_metadata(&fs_state) < 0) {
        debug_log("Failed to write metadata after truncate\n");
        return -EIO;
    }

    debug_log("Truncate successful: %s truncated to %ld bytes\n", path, size);
    return 0;
}

static int bmpfs_utimens(const char *path, const struct timespec ts[2],
                         struct fuse_file_info *fi) {
    int idx = path_to_metadata_index(path);
    if (idx < 0) {
        return idx;
    }

    FileMetadata *meta = &fs_state.files[idx];

    // Update access and modification times
    if (ts) {
        meta->accessed = ts[0].tv_sec;
        meta->modified = ts[1].tv_sec;
    } else {
        time_t current = time(NULL);
        meta->accessed = current;
        meta->modified = current;
    }

    debug_log("Updated timestamps for file: %s\n", path);
    return 0;
}

static int bmpfs_fsync(const char *path, int datasync,
                       struct fuse_file_info *fi) {
    if (!fs_state.bmp_file) {
        return -EIO;
    }

    if (datasync) {
        return fdatasync(fileno(fs_state.bmp_file));
    } else {
        return fsync(fileno(fs_state.bmp_file));
    }
}

static int bmpfs_open(const char *path, struct fuse_file_info *fi) {
    // Validate the path
    int idx = path_to_metadata_index(path);
    if (idx < 0) {
        return idx; // Return error code (e.g., -ENOENT)
    }

    FileMetadata *meta = &fs_state.files[idx];

    // Não é possível abrir diretórios para escrita
    if (meta->is_dir && (fi->flags & O_WRONLY)) {
        return -EACCES;
    }

    // Check read/write permissions based on mode
    if ((fi->flags & O_WRONLY) && !(meta->mode & S_IWUSR)) {
        return -EACCES; // Sem permissão de escrita
    }
    if ((fi->flags & O_RDONLY) && !(meta->mode & S_IRUSR)) {
        return -EACCES; // Sem permissão de leitura
    }

    // Update access time
    meta->accessed = time(NULL);

    debug_log("File opened successfully: %s\n", path);
    return 0; // File opened successfully
}

// Função para remover diretórios
static int bmpfs_rmdir(const char *path) {
    int idx = path_to_metadata_index(path);
    if (idx < 0) {
        return idx;
    }

    FileMetadata *meta = &fs_state.files[idx];

    // Verificar se é um diretório
    if (!meta->is_dir) {
        debug_log("Cannot remove a file as directory: %s\n", path);
        return -ENOTDIR;
    }

    // Verificar se o diretório está vazio
    for (size_t i = 0; i < fs_state.max_files; i++) {
        if (fs_state.files[i].filename[0] != '\0' &&
            strcmp(fs_state.files[i].filename, path) != 0) {
            // Para simplificar, assumimos que todos os arquivos estão na raiz
            // Se desejar suportar diretórios aninhados, isso precisaria ser ajustado
            continue;
        }
    }

    // Liberar blocos no bitmap, se houver
    for (uint32_t i = 0; i < meta->num_blocks; i++) {
        fs_state.bitmap[meta->first_block + i] = 0;
    }

    // Clear metadata
    memset(meta, 0, sizeof(FileMetadata));

    // Write updated metadata to the BMP file
    if (write_metadata(&fs_state) < 0) {
        debug_log("Failed to write metadata after directory deletion\n");
        return -EIO;
    }

    debug_log("Directory deleted successfully: %s (idx: %d)\n", path, idx);
    return 0;
}

// Define FUSE operations
static const struct fuse_operations bmpfs_ops = {
    .init       = bmpfs_init,
    .destroy    = bmpfs_destroy,
    .getattr    = bmpfs_getattr,
    .readdir    = bmpfs_readdir,
    .create     = bmpfs_create,
    .unlink     = bmpfs_unlink,
    .read       = bmpfs_read,
    .write      = bmpfs_write,
    .open       = bmpfs_open,
    .truncate   = bmpfs_truncate,
    .utimens    = bmpfs_utimens,
    .fsync      = bmpfs_fsync,
    .mkdir      = bmpfs_mkdir, // Adicionando a função mkdir
    .rmdir      = bmpfs_rmdir, // Adicionando a função rmdir
};

int main(int argc, char *argv[]) {
    struct fuse_args args = FUSE_ARGS_INIT(argc, argv);

    // Definir padrões
    config.image_path = NULL;

    // Analisar opções
    if (fuse_opt_parse(&args, &config, bmpfs_opts, NULL) == -1) {
        return 1;
    }

    // Verificar se o caminho da imagem foi fornecido
    if (config.image_path == NULL) {
        fprintf(stderr,
                "Usage: %s [FUSE options] mountpoint -o image=<image_file.bmp>\n",
                argv[0]);
        fuse_opt_free_args(&args);
        return 1;
    }

    // Armazenar caminho da imagem no estado do sistema de arquivos
    fs_state.image_path = strdup(config.image_path);
    if (!fs_state.image_path) {
        fprintf(stderr, "Failed to allocate memory for image path\n");
        fuse_opt_free_args(&args);
        return 1;
    }

    // Executar FUSE
    int ret = fuse_main(args.argc, args.argv, &bmpfs_ops, NULL);

    // Não liberar config.image_path aqui, pois foi duplicado
    // free(config.image_path); // Removido

    fuse_opt_free_args(&args);

    return ret;
}

