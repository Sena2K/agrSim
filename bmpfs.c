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

static void debug_log(const char *format, ...) {
  va_list args;
  va_start(args, format);
  vfprintf(stderr, format, args);
  va_end(args);
}

struct bmpfs_config {
  char *image_path;
};

static struct bmpfs_config config; // Global config instance

static struct fuse_opt bmpfs_opts[] = {BMPFS_OPT("image=%s", image_path),
                                       FUSE_OPT_END};

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

// File system metadata structure
typedef struct {
  char filename[256];
  size_t size;
  time_t created;
  time_t modified;
  time_t accessed;      // Added access time
  uint32_t first_block; // First data block index
  uint32_t num_blocks;  // Number of blocks used
  mode_t mode;          // Added proper mode support
  uid_t uid;            // Added user ID
  gid_t gid;            // Added group ID
} FileMetadata;

// File system state
typedef struct {
  FILE *bmp_file;
  BMPHeader header;
  BMPInfoHeader info_header;
  size_t data_size;    // Total available data size
  size_t block_size;   // Size of each block (in pixels)
  uint8_t *bitmap;     // Changed to uint8_t for better memory usage
  FileMetadata *files; // Array of file metadata
  size_t max_files;    // Maximum number of files
  char *image_path;    // Added to store image path
} bmp_fs_state;

static bmp_fs_state fs_state;

static size_t calculate_metadata_size(bmp_fs_state *state) {
  size_t total_blocks = state->data_size / state->block_size;
  size_t bitmap_size = total_blocks; // 1 byte per block
  size_t file_metadata_size = state->max_files * sizeof(FileMetadata);
  return bitmap_size + file_metadata_size;
}

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
    debug_log("Failed to read metadata area\n");
    free(buffer);
    return -EIO;
  }

  // Assign bitmap and file metadata
  size_t bitmap_size = state->data_size / state->block_size;
  memcpy(state->bitmap, buffer, bitmap_size);
  memcpy(state->files, buffer + bitmap_size,
         state->max_files * sizeof(FileMetadata));

  free(buffer);
  return 0;
}

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
  memcpy(buffer + bitmap_size, state->files,
         state->max_files * sizeof(FileMetadata));

  // Seek to the start of pixel data
  if (fseek(state->bmp_file, state->header.dataOffset, SEEK_SET) != 0) {
    debug_log("Failed to seek to metadata area\n");
    free(buffer);
    return -EIO;
  }

  // Write metadata
  size_t written = fwrite(buffer, 1, metadata_size, state->bmp_file);
  if (written != metadata_size) {
    debug_log("Failed to write metadata area\n");
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

// Create new BMP file with proper error handling
static int create_bmp_file(const char *filename, size_t width, size_t height) {
  debug_log("Attempting to create/open BMP file: %s\n", filename);

  // Check directory permissions first
  char *dir_path = strdup(filename);
  char *last_slash = strrchr(dir_path, '/');
  if (last_slash) {
    *last_slash = '\0';
  } else {
    dir_path[0] = '.';
    dir_path[1] = '\0';
  }

  // Check if we can write to the directory
  if (access(dir_path, W_OK) != 0) {
    debug_log("Directory %s is not writable (errno: %d - %s)\n", dir_path,
              errno, strerror(errno));
    free(dir_path);
    return -errno;
  }
  free(dir_path);

  // Check if file already exists
  if (access(filename, F_OK) == 0) {
    // File exists, check if we can write to it
    if (access(filename, W_OK) != 0) {
      debug_log("Existing file %s is not writable (errno: %d - %s)\n", filename,
                errno, strerror(errno));
      return -errno;
    }
    debug_log("File already exists and is writable\n");
    return 0;
  }

  // Try to create the file with explicit permissions
  int fd = open(filename, O_WRONLY | O_CREAT, 0644);
  if (fd == -1) {
    debug_log("Failed to create file with open() (errno: %d - %s)\n", errno,
              strerror(errno));
    return -errno;
  }
  FILE *fp = fdopen(fd, "wb");
  if (!fp) {
    debug_log("Failed to create FILE* from fd (errno: %d - %s)\n", errno,
              strerror(errno));
    close(fd);
    return -errno;
  }

  // Calculate sizes with overflow checking
  size_t pixel_data_size;
  if (__builtin_mul_overflow(width * height, 3, &pixel_data_size)) {
    fclose(fp);
    return -EOVERFLOW;
  }

  size_t file_size;
  if (__builtin_add_overflow(sizeof(BMPHeader) + sizeof(BMPInfoHeader),
                             pixel_data_size, &file_size)) {
    fclose(fp);
    return -EOVERFLOW;
  }

  // Create and write headers
  BMPHeader header = {.signature = 0x4D42,
                      .filesize = file_size,
                      .reserved1 = 0,
                      .reserved2 = 0,
                      .dataOffset = sizeof(BMPHeader) + sizeof(BMPInfoHeader)};

  BMPInfoHeader info_header = {.headerSize = sizeof(BMPInfoHeader),
                               .width = width,
                               .height = height,
                               .planes = 1,
                               .bitsPerPixel = 24,
                               .compression = 0,
                               .imageSize = pixel_data_size,
                               .xPixelsPerM = 2835,
                               .yPixelsPerM = 2835,
                               .colorsUsed = 0,
                               .colorsImportant = 0};

  if (fwrite(&header, sizeof(BMPHeader), 1, fp) != 1 ||
      fwrite(&info_header, sizeof(BMPInfoHeader), 1, fp) != 1) {
    debug_log("Failed to write headers (errno: %d - %s)\n", errno,
              strerror(errno));
    fclose(fp);
    return -errno;
  }

  // Initialize pixel data
  unsigned char *pixel_data = calloc(1, pixel_data_size);
  if (!pixel_data) {
    fclose(fp);
    return -ENOMEM;
  }

  size_t written = fwrite(pixel_data, 1, pixel_data_size, fp);
  free(pixel_data);

  if (written != pixel_data_size) {
    debug_log("Failed to write pixel data (errno: %d - %s)\n", errno,
              strerror(errno));
    fclose(fp);
    return -errno;
  }

  // Initialize bitmap and files structures
  size_t total_blocks = pixel_data_size / 512;
  size_t bitmap_size = total_blocks;
  size_t files_size = 1000 * sizeof(FileMetadata);
  size_t metadata_size = bitmap_size + files_size;

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
    debug_log("Failed to write initial metadata (errno: %d - %s)\n", errno,
              strerror(errno));
    fclose(fp);
    return -EIO;
  }

  // Flush changes to disk
  if (fflush(fp) != 0) {
    debug_log("Failed to flush BMP file after creation (errno: %d - %s)\n",
              errno, strerror(errno));
    fclose(fp);
    return -EIO;
  }

  debug_log("Successfully created BMP file with initial metadata\n");
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

  // Check for invalid characters
  const char *invalid = strchr(path, '/');
  if (invalid) {
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
    return -EIO;
  }

  size_t bytes_read =
      fread(buffer, 1, fs_state.block_size * num_blocks, fs_state.bmp_file);
  if (bytes_read != fs_state.block_size * num_blocks) {
    return -EIO;
  }

  return 0;
}

static int write_blocks(uint32_t start_block, size_t num_blocks,
                        const char *buffer) {
  if (!buffer || !fs_state.bmp_file) {
    return -EINVAL;
  }

  size_t metadata_size = calculate_metadata_size(&fs_state);
  size_t offset = fs_state.header.dataOffset + metadata_size +
                  (start_block * fs_state.block_size);
  if (fseek(fs_state.bmp_file, offset, SEEK_SET) != 0) {
    return -EIO;
  }

  size_t bytes_written =
      fwrite(buffer, 1, fs_state.block_size * num_blocks, fs_state.bmp_file);
  if (bytes_written != fs_state.block_size * num_blocks) {
    return -EIO;
  }

  if (fflush(fs_state.bmp_file) != 0) {
    return -EIO;
  }

  return 0;
}

// FUSE operations with improved error handling and security
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
  stbuf->st_nlink = 1;
  stbuf->st_size = meta->size;
  stbuf->st_uid = meta->uid;
  stbuf->st_gid = meta->gid;
  stbuf->st_atime = meta->accessed;
  stbuf->st_mtime = meta->modified;
  stbuf->st_ctime = meta->created;
  stbuf->st_blocks = (meta->size + 511) / 512; // Standard block size
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

  // Check if file already exists
  if (path_to_metadata_index(path) >= 0) {
    debug_log("File already exists\n");
    return -EEXIST;
  }

  // Find empty metadata slot
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

  debug_log("File created successfully: %s (idx: %d)\n", path, idx);

  // Write updated metadata to BMP file
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
    debug_log("Could not open existing file (errno: %d - %s)\n", errno,
              strerror(errno));

    int create_result = create_bmp_file(fs_state.image_path, 2048, 2048);
    if (create_result < 0) {
      debug_log("Failed to create BMP file: %d (errno: %d - %s)\n",
                create_result, errno, strerror(errno));
      return NULL;
    }

    fs_state.bmp_file = fopen(fs_state.image_path, "r+b");
    if (!fs_state.bmp_file) {
      debug_log("Failed to open created BMP file (errno: %d - %s)\n", errno,
                strerror(errno));
      return NULL;
    }
  }

  // Verify permissions
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

  if (header.signature != 0x4D42) {
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
  fs_state.data_size = info_header.imageSize;
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
  // Write metadata to BMP file
  if (write_metadata(&fs_state) < 0) {
    debug_log("Failed to write metadata on destroy\n");
  }

  // Close the BMP file if it's open
  if (fs_state.bmp_file) {
    fclose(fs_state.bmp_file);
    fs_state.bmp_file = NULL;
  }

  // Free allocated memory for the bitmap, files, and image path
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

  // Free blocks in bitmap
  for (uint32_t i = 0; i < meta->num_blocks; i++) {
    fs_state.bitmap[meta->first_block + i] = 0;
  }

  // Clear metadata
  memset(meta, 0, sizeof(FileMetadata));

  // Write updated metadata to BMP file
  if (write_metadata(&fs_state) < 0) {
    debug_log("Failed to write metadata after file deletion\n");
    return -EIO;
  }

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

  // Update access time
  meta->accessed = time(NULL);

  if (offset >= meta->size) {
    return 0;
  }

  // Adjust size if reading past end of file
  if (offset + size > meta->size) {
    size = meta->size - offset;
  }

  // Calculate block positions
  uint32_t start_block = meta->first_block + (offset / fs_state.block_size);
  size_t block_offset = offset % fs_state.block_size;
  size_t blocks_to_read =
      (size + block_offset + fs_state.block_size - 1) / fs_state.block_size;

  // Allocate temporary buffer for block-aligned reads
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

  return size;
}

static int bmpfs_write(const char *path, const char *buf, size_t size,
                       off_t offset, struct fuse_file_info *fi) {
  debug_log("Writing to file: %s (size: %zu, offset: %ld)\n", path, size,
            offset);

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
  size_t new_size = offset + size;

  // Check for overflow
  if (new_size < offset) {
    debug_log("File size overflow\n");
    return -EFBIG;
  }

  // Calculate required blocks
  size_t new_blocks =
      (new_size + fs_state.block_size - 1) / fs_state.block_size;
  debug_log("Required blocks: %zu (current: %u)\n", new_blocks,
            meta->num_blocks);

  // If we need more blocks
  if (new_blocks > meta->num_blocks) {
    uint32_t new_start = find_free_blocks(new_blocks);
    if (new_start == UINT32_MAX) {
      debug_log("No free blocks available\n");
      return -ENOSPC;
    }

    debug_log("Allocated new blocks starting at: %u\n", new_start);

    // If we already have blocks, copy existing data
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

  // If not writing a complete block, read existing data first
  if (block_offset > 0 || (size % fs_state.block_size) != 0) {
    int read_result = read_blocks(start_block, blocks_to_write, temp_buf);
    if (read_result < 0) {
      debug_log("Failed to read blocks for partial write: %d\n", read_result);
      free(temp_buf);
      return read_result;
    }
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

  // Write updated metadata to BMP file
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

  // Add standard entries
  if (filler(buf, ".", NULL, 0, 0) || filler(buf, "..", NULL, 0, 0)) {
    return -ENOMEM;
  }

  // Add all non-empty file entries
  for (size_t i = 0; i < fs_state.max_files; i++) {
    if (fs_state.files[i].filename[0] != '\0') {
      struct stat st;
      memset(&st, 0, sizeof(struct stat));
      st.st_mode = fs_state.files[i].mode;
      st.st_nlink = 1; // Regular files have at least one link
      st.st_size = fs_state.files[i].size;
      st.st_uid = fs_state.files[i].uid;
      st.st_gid = fs_state.files[i].gid;
      st.st_atime = fs_state.files[i].accessed;
      st.st_mtime = fs_state.files[i].modified;
      st.st_ctime = fs_state.files[i].created;
      st.st_blocks =
          (fs_state.files[i].size + 511) / 512; // Number of 512-byte blocks
      st.st_blksize = fs_state.block_size;

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

  // Calculate new block count
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
  // If shrinking
  else if (new_blocks < meta->num_blocks) {
    for (uint32_t i = new_blocks; i < meta->num_blocks; i++) {
      fs_state.bitmap[meta->first_block + i] = 0;
    }
    meta->num_blocks = new_blocks;
    meta->size = size;
    meta->modified = time(NULL);
  }
  // If growing
  else if (new_blocks > meta->num_blocks) {
    uint32_t new_start = find_free_blocks(new_blocks);
    if (new_start == UINT32_MAX) {
      return -ENOSPC;
    }

    // Copy existing data
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

  // Write updated metadata to BMP file
  if (write_metadata(&fs_state) < 0) {
    debug_log("Failed to write metadata after truncate\n");
    return -EIO;
  }

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

  // Check if the file has read/write permissions as per the mode
  if ((fi->flags & O_WRONLY) && !(meta->mode & S_IWUSR)) {
    return -EACCES; // No write permission
  }
  if ((fi->flags & O_RDONLY) && !(meta->mode & S_IRUSR)) {
    return -EACCES; // No read permission
  }

  // Update file access time
  meta->accessed = time(NULL);

  return 0; // Successfully opened the file
}

static const struct fuse_operations bmpfs_ops = {
    .init = bmpfs_init,
    .destroy = bmpfs_destroy,
    .getattr = bmpfs_getattr,
    .readdir = bmpfs_readdir,
    .create = bmpfs_create,
    .unlink = bmpfs_unlink,
    .read = bmpfs_read,
    .write = bmpfs_write,
    .open = bmpfs_open,
    .truncate = bmpfs_truncate,
    .utimens = bmpfs_utimens,
    .fsync = bmpfs_fsync,
};

int main(int argc, char *argv[]) {
  struct fuse_args args = FUSE_ARGS_INIT(argc, argv);

  // Set defaults
  config.image_path = NULL;

  // Parse options
  if (fuse_opt_parse(&args, &config, bmpfs_opts, NULL) == -1) {
    return 1;
  }

  // Check if image path was provided
  if (config.image_path == NULL) {
    fprintf(stderr,
            "Usage: %s [FUSE options] mountpoint -o image=<image_file.bmp>\n",
            argv[0]);
    fuse_opt_free_args(&args);
    return 1;
  }

  // Store image path in filesystem state
  fs_state.image_path = strdup(config.image_path);
  if (!fs_state.image_path) {
    fprintf(stderr, "Failed to allocate memory for image path\n");
    fuse_opt_free_args(&args);
    return 1;
  }

  // Run FUSE
  int ret = fuse_main(args.argc, args.argv, &bmpfs_ops, NULL);

  // Cleanup
  // Remove the incorrect free call
  // free(config.image_path); // Incorrect: Remove or comment out

  fuse_opt_free_args(&args);

  return ret;
}
