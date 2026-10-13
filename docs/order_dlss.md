# DLSS em The Order: 1886

`BB_ORDER_DLSS=1` (com `BB_DLSS=1`, `BB_UPSCALER=none` e as DLLs `bbport_dlss.dll` +
`nvngx_dlss.dll` ao lado do executável) troca o resolve temporal MSAA do jogo pelo DLSS.
Código: `vk_order_dlss.{h,cpp}`, shader `host_shaders/order_dlss_prep.comp`.

## Por que este ponto

Palestra da Ready at Dawn (Matt Pettineo, "Rendering The Alternate History of The Order: 1886",
SIGGRAPH 2015 Advances, slides 3, 18, 21-24 e 33):
- o prepass grava profundidade, normal de vértice e **velocidade**;
- AA = EQAA (2 fragmentos de cor) + resolve próprio em compute com filtro cúbico e peso
  `1 / (1 + luminância * exposição)`, mais TAA integrado ao resolve, que reprojeta pela
  velocidade e limita a vizinhança a min/max;
- **sem jitter** ("não deu tempo de integrar");
- o tonemap vem depois do pós-processamento (DoF, bloom), que trabalha em HDR.

No port, esse resolve é `customresolve_cs_msaa4x_temporal` (`0x92311aca`; a variante 2x é
`0xe57f0150`). Na captura run95 (#71 de cerca de 75 passes) ele recebe:

| Binding | Imagem | Conteúdo |
| --- | --- | --- |
| img0 | R32F 1x1 | log2 da exposição (lido quando flatbuf[61] != 0; senão flatbuf[60]) |
| img1 | A2B10G10R10 1280x720 (color0 do prepass) | velocidade: rg = \|x\|,\|y\|; a*3 = bits de sinal |
| img2 | RGBA16F 1280x720 2x MSAA | cena HDR |
| img3 | RGBA16F 1280x720 | histórico |
| img4 (storage) | RGBA16F 1280x720 | saída, que vira o histórico do quadro seguinte |

A decodificação da velocidade foi tirada do SPIR-V do resolve:
`vx = -r * (bit0 ? -1 : 1)`, `vy = g * (bit1 ? 1 : -1)`, posição anterior = pixel + v * flatbuf[63].
Ou seja, o vetor em pixels já está na convenção que o DLSS espera (anterior - atual, escala 1).

## O que a integração faz

1. Lembra o alvo de profundidade desenhado junto com a cor MSAA RGBA16F (`OnTargets`).
2. No dispatch do resolve, reconhece as imagens pelo formato e confere tamanhos e amostras.
   Se algo não bater, deixa o resolve do jogo rodar e loga o motivo (até 8 vezes).
3. O compute de preparação gera:
   - a cor resolvida com o mesmo peso inverso de luminância do jogo;
   - os vetores em RG16F;
   - a profundidade (amostra 0) em R32F.
4. O DLSS roda em DLAA, com entrada e saída do mesmo tamanho, HDR e exposição automática.
   - Jitter 0, porque o jogo não tem.
   - O histórico é reiniciado após uma pausa maior que 250 ms.
5. Copia o resultado para a imagem de saída do jogo e pula o dispatch original. DoF, bloom,
   foco e tonemap seguem iguais.
6. Se o DLSS falhar, desliga só o DLSS da sessão e o resolve do jogo assume.

`BB_ORDER_DLSS_DEPTH_INVERT=1` inverte a profundidade, caso a convenção Z se mostre invertida.

## Limites conhecidos

- Só DLAA. Reduzir a resolução interna (modo Qualidade) exige:
  - renderizar a cena menor;
  - adaptar os alvos MSAA, o viewport e os passes que assumem 1280x720;
  - mover a saída para o tamanho final.
  A ordem DLAA primeiro, Qualidade depois separa defeitos de reprojeção dos defeitos de escala.
- Sem jitter, o DLSS estabiliza a imagem mas recupera menos detalhe do que poderia.
  - Adicionar jitter exige alterar a projeção do jogo nas constantes de câmera.
  - O DoF (CoC pela profundidade) e o foco da mira também teriam de ser revistos.
- A profundidade não tem jitter e usa a amostra 0. O DoF do jogo usa o menor CoC entre as
  amostras, o que não afeta o DLSS.
