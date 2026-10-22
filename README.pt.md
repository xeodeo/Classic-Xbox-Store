# Classic Xbox Store

[Español](README.es.md) · [English](README.md) · **Português**

<p align="center"><img src="docs/img/icono.png" alt="Classic Xbox Store" width="200"></p>

Loja de jogos **nativa para o Xbox original** (2001): um `default.xbe` que abre pelo painel de um console desbloqueado, mostra o catálogo de jogos de Xbox do Internet Archive com as capas, **baixa e instala os jogos direto no HD do console** e abre eles pela própria lista. Sem PC no meio.

É a versão para console do [Classic Xbox Loader](https://github.com/xeodeo) (o app de PC que baixa, extrai e envia os jogos por FTP), feita com o mesmo sistema de compilação do port do [OptiCraft para o Xbox](https://github.com/xeodeo/OptiCraft-Heritage-XBOX-CLASSIC-Edition).

| Loja | Download |
|---|---|
| ![Loja](docs/img/tienda.png) | ![Download](docs/img/descarga.png) |
| **Rede e proxies** | **Conta (teclado na tela)** |
| ![Rede](docs/img/red.png) | ![Conta](docs/img/cuenta.png) |

## O que faz

- **Loja:** catálogo do Internet Archive por letra (`#`, `A`…`Z`), com região e tamanho. O jogo selecionado mostra a **capa** à direita e o **fundo** atrás, com transição. As listas e imagens ficam salvas em `T:\cache`, então na segunda vez aparecem na hora e continuam visíveis mesmo com o archive.org fora do ar.
- **Instalar (A):** escolha a unidade (`E:`, `F:` ou `G:`, com o espaço livre real) e ela:
  1. baixa o **ZIP** do archive.org com **16 conexões ao mesmo tempo** (cada pedaço com `Range`) e descompacta no console com zlib;
  2. pula a partição de vídeo da imagem redump (os primeiros 387 MB) e guarda a do jogo em pedaços de menos de 4 GB (limite do FATX);
  3. assim que chega a árvore de arquivos do disco (XDVDFS), sabe onde termina o último arquivo e **para o download ali**: o preenchimento do final não é baixado;
  4. copia os arquivos para `<unidade>:\Games\<jogo>` (o `default.xbe` por último), confere se cada um tem o tamanho do disco original e apaga os temporários.
- **Continuar o que ficou pela metade:** a cada 128 MB é salvo um ponto de controle (incluindo o estado do zlib, como o exemplo `zran` do zlib). Se o console desligar, você sair com a combinação de botões ou a rede cair, ao abrir a loja ela pergunta se quer continuar. O mesmo se parar durante a cópia.
- **Instalados (Y):** jogos e aplicativos do HD (qualquer pasta de primeiro nível com subpastas que tenham `default.xbe`), separados em *Jogos* e *Aplicativos*. **A abre.**
- **Conta (Start):** entrar no archive.org pelo console com um teclado na tela (o archive.org não deixa baixar essas coleções sem conta). A senha nunca é salva: só os cookies que o archive.org devolve, em `T:\ia_session.json`. O `ia_session.json` do Classic Xbox Loader copiado junto ao `default.xbe` também funciona.
- **Rede (X):** diagnóstico (IP, DNS, conexões, HTTPS) e as últimas linhas do registro. **Y abre Proxies:** buscar proxies grátis próximos (A) e ligar ou desligar (Y). Vêm **desligados por padrão**; ligados, metade das conexões passa por eles, a lista se renova sozinha a cada 3 horas e uma conexão cujo proxy falha continua direta.
- **Idiomas:** inglês (padrão), espanhol e português. Troque em Conta (Start) com Y; a escolha fica salva.

## Como funciona por dentro

- **HTTPS no Xbox:** [mbedTLS](https://github.com/Mbed-TLS/mbedtls) 3.6 (só cliente TLS 1.2, sem instruções que o Pentium III não tem) sobre os sockets do XNet. Os certificados raiz necessários vão dentro do XBE (`assets/certs/roots.pem`).
- **Velocidade:** a pilha TCP do Xbox não amplia a janela (64 KB no máximo por conexão); com ~0,2 s de ida e volta até o archive.org isso dá ~0,3 MB/s por conexão. Por isso pedaços de 1 MB são distribuídos entre 16 conexões reutilizadas (keep-alive); num console real chega a 3–5 MB/s.
- **DNS:** se o DNS configurado pelo painel não responder, pergunta a 1.1.1.1 e 8.8.8.8, e guarda os endereços.
- **Capas:** o índice `assets/data/artindex.tsv` (gerado com `scripts/make_art_index.py`) liga o nome redump de cada ZIP ao seu XMID e Title ID e diz quais artes existem, para não pedir imagens que não há.
- **Registro:** `E:\TDATA\FFFF4358\cxs_log.txt` (pegue por FTP pelo painel). Se uma thread falhar, anota a exceção, os registradores e a pilha com endereços do `.map`.

## De onde vêm os jogos

Do catálogo de jogos de Xbox do Internet Archive, as mesmas coleções que o Classic Xbox Loader usa:

- `https://archive.org/details/microsoft_xbox_numberssymbols` (#)
- `https://archive.org/details/microsoft_xbox_a` … `microsoft_xbox_z` (algumas letras em duas partes: `microsoft_xbox_c_part1` e `_part2`, igual para D, M, N, O, S e T)

A lista de cada letra vem de `https://archive.org/metadata/<coleção>/files` e cada jogo é baixado de `https://archive.org/download/<coleção>/<jogo>.zip`. É preciso uma conta no archive.org.

Este repositório não hospeda nem distribui nenhum jogo.

## Compilar

### O que é preciso

- **Windows 10/11** com **Visual Studio 2022** (carga de trabalho *Desenvolvimento para desktop com C++*, com as ferramentas x86) e o **Windows 10/11 SDK** (para o UCRT estático).
- **Python 3** (coloca as fontes, os certificados e o índice de capas dentro do XBE).
- **O XDK do Xbox (5849).** **Não está neste repositório e nunca deve ser enviado.** É o mesmo que o OptiCraft usa, preservado na coleção do Internet Archive <https://archive.org/details/xbox-sdks> (arquivo `2005-03 - 5849.6, 5849.17 - RecoveryEXE, SDK.7z`).
  1. Baixe esse arquivo e extraia com o 7-Zip: dentro está `XDKSetup5849.17.exe`.
  2. Extraia também o instalador com o 7-Zip (não precisa instalar; ele pede Visual Studio .NET 2003 e Windows XP):

     ```text
     7z x XDKSetup5849.17.exe -o<xdk>\sdk
     ```

  3. O XDK fica em `<xdk>\sdk\XDK\xbox` (`bin\`, `include\`, `lib\`). Essa é a pasta usada: o linker do XDK (`bin\vc71\Link.Exe`), `imagebld.exe`, `bundler.exe` e as bibliotecas XAPI, Direct3D 8 e XNet.
- `scripts/ninja.exe` e `scripts/extract-xiso.exe` já vêm no repositório.

Testado com Windows 10 Pro 22H2, Visual Studio 2022 Community (MSVC 14.44), Windows SDK 10.0.26100, CMake 3.31 (o do Visual Studio), Ninja 1.13.2, XDK 5849, extract-xiso 2.7.1, [xemu](https://xemu.app) e um console retail com softmod.

### Comandos

No PowerShell:

```powershell
$env:XBOX_XDK_ROOT = "C:\caminho\para\xdk\5849\sdk\XDK\xbox"
$cmake = "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
& $cmake --preset xbox
& $cmake --build --preset xbox
```

Sai em `bin/`:

- `bin/iso/` com `default.xbe` e `default.tbn` (a miniatura para painéis tipo XBMC/UnleashX). **Essa pasta é a que se copia para o console**, por exemplo para `F:\Apps\Classic Xbox Store\`.
- `bin/ClassicXboxStore.iso`, o mesmo como ISO para o xemu.

O preset `xbox-autotest` compila uma versão de teste para o xemu (em `bin-autotest/`) que instala sozinha um ZIP servido pelo PC (`10.0.2.2:8000`, um servidor HTTP com `Range`), sem depender do archive.org.

### Como é compilado (o mesmo truque do OptiCraft)

O compilador do XDK é o Visual C++ 7.1 (2003), que não entende C++17. Então dois são combinados:

- **Compilar:** o `cl.exe` do VS2022 (x86, `/arch:SSE`: o Pentium III tem SSE mas não SSE2) com a STL e o UCRT modernos (`cmake/xbox_toolchain.cmake`).
- **Linkar:** o `Link.Exe` do XDK, com o CRT estático moderno (`libcmt`, `libcpmt`, `libucrt`) e depois as bibliotecas do XDK. `src/runtime/XboxCrtShim.cpp` fornece as funções Win32 que o CRT moderno precisa e o XAPI não tem, e `src/runtime/XboxEntry.cpp` repete a inicialização do XDK.
- **Depois:** `scripts/patch_pe_for_imagebld.ps1` prepara o PE para o `imagebld`, `scripts/patch_sse2_moves.ps1` troca as instruções SSE2 deixadas pelo CRT por equivalentes que o Pentium III suporta, `bundler.exe` empacota o ícone e `imagebld.exe` cria o `default.xbe`.

### Dados dentro do XBE

| Arquivo | O que é | Como regenerar |
|---|---|---|
| `assets/fonts/OpenSans-*.ttf` | Fonte da interface | — |
| `assets/certs/roots.pem` | Certificados raiz (Let's Encrypt/ISRG, GoDaddy, DigiCert, USERTrust, GlobalSign) | do `certifi` |
| `assets/data/artindex.tsv` | Índice de capas (nome redump → XMID, Title ID e artes disponíveis) | `python scripts/make_art_index.py` |
| `assets/icon/` | Ícone do painel, dos saves e `default.tbn` | `python scripts/make_icon.py` |
| `src/ui/LangTable.inc` | Traduções (inglês e português) dos textos, que no código estão em espanhol | `python scripts/make_lang.py` (a partir de `scripts/translations.py`) |

## Usar no console

1. Copie o conteúdo de `bin/iso/` (ou do zip da release) para uma pasta do HD (por FTP pelo painel), por exemplo `F:\Apps\Classic Xbox Store\`.
2. Abra pelo painel.
3. Entre na conta com **Start** (ou copie o `ia_session.json` do Classic Xbox Loader junto ao `default.xbe`).

| Botão | Loja | Instalados | Rede |
|---|---|---|---|
| A | instalar | abrir | testar a rede (em Proxies: buscar) |
| Y | ir para Instalados | ir para a Loja | abrir Proxies (em Proxies: ligar / desligar) |
| X | Rede | Rede | voltar |
| Start | Conta (Y: idioma) | Conta | Conta |
| Esquerda / direita | letra | — | — |
| Gatilhos | página | página | — |
| B | voltar ao painel | voltar ao painel | voltar ao painel |

## Créditos

- **Capas, fundos e ícones dos jogos:** banco de dados e arte de **MobCat** — <https://mobcat.zip/XboxIDs/> e <https://github.com/MobCat/MobCats-original-xbox-game-list>. A loja baixa de lá; não estão incluídos aqui.
- **Jogos:** coleções de Xbox do [Internet Archive](https://archive.org).
- **Lista de proxies:** [ProxyScrape](https://proxyscrape.com).
- [mbedTLS](https://github.com/Mbed-TLS/mbedtls) (Apache 2.0), [zlib](https://zlib.net) (licença zlib), [stb_image e stb_truetype](https://github.com/nothings/stb) (domínio público / MIT), [Open Sans](https://fonts.google.com/specimen/Open+Sans) (Apache 2.0), [extract-xiso](https://github.com/XboxDev/extract-xiso) e [Ninja](https://ninja-build.org).
- Sistema de compilação e inicialização tirados do port do OptiCraft para o Xbox, de xeodeo.
- Estilo da interface inspirado no [PKGi](https://github.com/bucanero/pkgi-ps3).

Xbox é uma marca da Microsoft. Este projeto não tem relação com a Microsoft nem com o Internet Archive.
