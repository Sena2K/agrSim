// main.c

#include "bmpfs.h"
#include <fuse3/fuse.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Função principal
int main(int argc, char *argv[]) {
    struct fuse_args args = FUSE_ARGS_INIT(argc, argv);
    config_bmpfs.configuracao_caminho_imagem = NULL;

    // Analisar opções
    if (fuse_opt_parse(&args, &config_bmpfs, opcoes_bmpfs, NULL) == -1) {
        return 1;
    }

    // Verificar se o caminho da imagem foi fornecido
    if (config_bmpfs.configuracao_caminho_imagem == NULL) {
        fprintf(stderr,
                "Uso: %s [Opções FUSE] ponto_de_montagem -o imagem=<arquivo_imagem.bmp>\n",
                argv[0]);
        fuse_opt_free_args(&args);
        return 1;
    }

    // Armazenar caminho da imagem no estado do sistema de arquivos
    estado_sistema_bmpfs.caminho_imagem = strdup(config_bmpfs.configuracao_caminho_imagem);
    if (!estado_sistema_bmpfs.caminho_imagem) {
        fprintf(stderr, "Falha ao alocar memória para o caminho da imagem\n");
        fuse_opt_free_args(&args);
        return 1;
    }

    // Executar FUSE
    int retorno = fuse_main(args.argc, args.argv, &operacoes_bmpfs, NULL);

    fuse_opt_free_args(&args);
    return retorno;
}

