// bmp.c

#include "bmp.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdint.h>

// Função para criar um novo arquivo BMP
int criar_arquivo_bmp(const char *nome_arquivo, size_t largura, size_t altura) {
    FILE *fp = fopen(nome_arquivo, "wb");
    if (!fp) {
        fprintf(stderr, "Erro ao criar arquivo BMP '%s': %s\n", nome_arquivo, strerror(errno));
        return -errno;
    }

    // Calcular tamanho da linha com padding (cada linha é alinhada a múltiplos de 4 bytes)
    size_t tamanho_linha = (largura * 3 + 3) & ~3;
    size_t tamanho_dados_pixel = tamanho_linha * altura;

    // Calcular tamanho do arquivo
    size_t tamanho_arquivo = sizeof(CabeçalhoBMP) + sizeof(InfoCabecalhoBMP) + tamanho_dados_pixel;

    // Criar e escrever cabeçalhos
    CabeçalhoBMP cabecalho = {
        .assinatura = 0x4D42, // "BM" em little endian
        .tamanho_arquivo = tamanho_arquivo,
        .reservado1 = 0,
        .reservado2 = 0,
        .deslocamento_dados = sizeof(CabeçalhoBMP) + sizeof(InfoCabecalhoBMP)
    };

    InfoCabecalhoBMP info_cabecalho = {
        .tamanho_cabecalho = sizeof(InfoCabecalhoBMP),
        .largura = (int32_t)largura,
        .altura = (int32_t)altura,
        .planos = 1,
        .bits_por_pixel = 24,
        .compressao = 0,
        .tamanho_imagem = (uint32_t)tamanho_dados_pixel,
        .pixels_por_m_x = 2835,
        .pixels_por_m_y = 2835,
        .cores_usadas = 0,
        .cores_importantes = 0
    };

    if (fwrite(&cabecalho, sizeof(CabeçalhoBMP), 1, fp) != 1 ||
        fwrite(&info_cabecalho, sizeof(InfoCabecalhoBMP), 1, fp) != 1) {
        fprintf(stderr, "Erro ao escrever cabeçalhos BMP: %s\n", strerror(errno));
        fclose(fp);
        return -EIO;
    }

    // Inicializar dados de pixel com zeros
    unsigned char *dados_pixel = calloc(1, tamanho_dados_pixel);
    if (!dados_pixel) {
        fprintf(stderr, "Erro ao alocar memória para dados de pixel\n");
        fclose(fp);
        return -ENOMEM;
    }

    size_t escritos_pixels = fwrite(dados_pixel, 1, tamanho_dados_pixel, fp);
    free(dados_pixel);

    if (escritos_pixels != tamanho_dados_pixel) {
        fprintf(stderr, "Erro ao escrever dados de pixel: %s\n", strerror(errno));
        fclose(fp);
        return -EIO;
    }

    // Flush das mudanças para o disco
    if (fflush(fp) != 0) {
        fprintf(stderr, "Erro ao flush do arquivo BMP: %s\n", strerror(errno));
        fclose(fp);
        return -EIO;
    }

    fclose(fp);
    return 0;
}

// Função para ler cabeçalho BMP
int ler_cabecalho_bmp(FILE *fp, CabeçalhoBMP *cabecalho, InfoCabecalhoBMP *info_cabecalho) {
    if (fread(cabecalho, sizeof(CabeçalhoBMP), 1, fp) != 1) {
        fprintf(stderr, "Erro ao ler cabeçalho BMP: %s\n", strerror(errno));
        return -EIO;
    }

    if (cabecalho->assinatura != 0x4D42) { // "BM" em little endian
        fprintf(stderr, "Assinatura BMP inválida\n");
        return -EINVAL;
    }

    if (fread(info_cabecalho, sizeof(InfoCabecalhoBMP), 1, fp) != 1) {
        fprintf(stderr, "Erro ao ler info cabeçalho BMP: %s\n", strerror(errno));
        return -EIO;
    }

    return 0;
}

// Função para escrever cabeçalho BMP
int escrever_cabecalho_bmp(FILE *fp, const CabeçalhoBMP *cabecalho, const InfoCabecalhoBMP *info_cabecalho) {
    if (fwrite(cabecalho, sizeof(CabeçalhoBMP), 1, fp) != 1 ||
        fwrite(info_cabecalho, sizeof(InfoCabecalhoBMP), 1, fp) != 1) {
        fprintf(stderr, "Erro ao escrever cabeçalhos BMP: %s\n", strerror(errno));
        return -EIO;
    }

    return 0;
}

