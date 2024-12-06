#include "bmp.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdint.h>

int criar_arquivo_bmp(const char *nome, size_t larg, size_t alt) {
    FILE *f = fopen(nome, "wb");
    if (!f) return -errno;

    size_t tam_linha = (larg * 3 + 3) & ~3;
    size_t tam_pixels = tam_linha * alt;
    size_t tam_arquivo = sizeof(CabeçalhoBMP) + sizeof(InfoCabecalhoBMP) + tam_pixels;

    CabeçalhoBMP cab = {
        .assinatura = 0x4D42,
        .tamanho_arquivo = tam_arquivo,
        .reservado1 = 0,
        .reservado2 = 0,
        .deslocamento_dados = sizeof(CabeçalhoBMP) + sizeof(InfoCabecalhoBMP)
    };

    InfoCabecalhoBMP info = {
        .tamanho_cabecalho = sizeof(InfoCabecalhoBMP),
        .largura = larg,
        .altura = alt,
        .planos = 1,
        .bits_por_pixel = 24,
        .compressao = 0,
        .tamanho_imagem = tam_pixels,
        .pixels_por_m_x = 2835,
        .pixels_por_m_y = 2835,
        .cores_usadas = 0,
        .cores_importantes = 0
    };

    if (fwrite(&cab, sizeof(CabeçalhoBMP), 1, f) != 1 ||
        fwrite(&info, sizeof(InfoCabecalhoBMP), 1, f) != 1) {
        fclose(f);
        return -EIO;
    }

    unsigned char *pixels = calloc(1, tam_pixels);
    if (!pixels) {
        fclose(f);
        return -ENOMEM;
    }

    size_t escrito = fwrite(pixels, 1, tam_pixels, f);
    free(pixels);

    if (escrito != tam_pixels) {
        fclose(f);
        return -EIO;
    }

    if (fflush(f) != 0) {
        fclose(f);
        return -EIO;
    }

    fclose(f);
    return 0;
}

int ler_cabecalho_bmp(FILE *f, CabeçalhoBMP *cab, InfoCabecalhoBMP *info) {
    if (fread(cab, sizeof(CabeçalhoBMP), 1, f) != 1) return -EIO;
    if (cab->assinatura != 0x4D42) return -EINVAL;
    if (fread(info, sizeof(InfoCabecalhoBMP), 1, f) != 1) return -EIO;
    return 0;
}

int escrever_cabecalho_bmp(FILE *f, const CabeçalhoBMP *cab, const InfoCabecalhoBMP *info) {
    if (fwrite(cab, sizeof(CabeçalhoBMP), 1, f) != 1 ||
        fwrite(info, sizeof(InfoCabecalhoBMP), 1, f) != 1) return -EIO;
    return 0;
}
