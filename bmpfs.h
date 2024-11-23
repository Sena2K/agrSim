// bmpfs.h

#ifndef BMPFS_H
#define BMPFS_H

#define FUSE_USE_VERSION 31
#define BMPFS_OPT(t, p) {t, offsetof(struct config_bmpfs, p), 1}

#include <fuse3/fuse.h>
#include <stdint.h>
#include <stdio.h>
#include <stddef.h>
#include <time.h>
#include "bmp.h" // Inclui bmp.h para acessar CabeçalhoBMP e InfoCabecalhoBMP

// Estrutura de configuração
struct config_bmpfs {
    char *configuracao_caminho_imagem;
};

// Estrutura de metadados de arquivo
#pragma pack(push, 1)
typedef struct {
    char nome_arquivo[256];
    uint64_t tamanho;
    uint64_t criado;
    uint64_t modificado;
    uint64_t acessado;
    uint32_t primeiro_bloco;
    uint32_t num_blocos;
    uint32_t modo;
    uint32_t uid;
    uint32_t gid;
    uint8_t eh_diretorio; // 1 para diretório, 0 para arquivo
} MetadadosArquivo;
#pragma pack(pop)

// Verificação estática do tamanho da estrutura
_Static_assert(sizeof(MetadadosArquivo) == 309, "MetadadosArquivo deve ter 309 bytes");

// Estrutura de estado do sistema de arquivos
typedef struct {
    FILE *arquivo_bmp;
    CabeçalhoBMP cabecalho;
    InfoCabecalhoBMP info_cabecalho;
    size_t tamanho_dados;
    size_t tamanho_bloco;
    uint8_t *bitmap;
    MetadadosArquivo *arquivos;
    size_t max_arquivos;
    char *caminho_imagem;
} estado_bmpfs;

// Declaração da configuração global
extern struct config_bmpfs config_bmpfs;

// Declaração do estado global do sistema de arquivos
extern estado_bmpfs estado_sistema_bmpfs;

// Declaração das opções do FUSE
extern struct fuse_opt opcoes_bmpfs[];

// Declaração das operações do FUSE
extern struct fuse_operations operacoes_bmpfs;

// Nenhuma declaração de funções auxiliares aqui, pois são internas a bmpfs.c

#endif // BMPFS_H

