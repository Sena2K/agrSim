// bmp.h

#ifndef BMP_H
#define BMP_H

#include <stdint.h>
#include <stddef.h> // Necessário para size_t
#include <stdio.h>   // Necessário para FILE

// Estrutura de cabeçalho BMP (14 bytes)
#pragma pack(push, 1)
typedef struct {
    uint16_t assinatura;  
    uint32_t tamanho_arquivo;
    uint16_t reservado1;
    uint16_t reservado2;
    uint32_t deslocamento_dados;
} CabeçalhoBMP;
#pragma pack(pop)

// Estrutura de info cabeçalho BMP (40 bytes)
#pragma pack(push, 1)
typedef struct {
    uint32_t tamanho_cabecalho;
    int32_t largura;
    int32_t altura;
    uint16_t planos;
    uint16_t bits_por_pixel;
    uint32_t compressao;
    uint32_t tamanho_imagem;
    int32_t pixels_por_m_x;
    int32_t pixels_por_m_y;
    uint32_t cores_usadas;
    uint32_t cores_importantes;
} InfoCabecalhoBMP;
#pragma pack(pop)

// Declarações das funções relacionadas ao BMP
int criar_arquivo_bmp(const char *nome_arquivo, size_t largura, size_t altura);
int ler_cabecalho_bmp(FILE *fp, CabeçalhoBMP *cabecalho, InfoCabecalhoBMP *info_cabecalho);
int escrever_cabecalho_bmp(FILE *fp, const CabeçalhoBMP *cabecalho, const InfoCabecalhoBMP *info_cabecalho);

#endif // BMP_H

