/*
 * Space Invaders
 * --------------
 * Versão simplificada do clássico Space Invaders na OLED 128x64. Uma
 * grade de inimigos (3 fileiras x 6 colunas, cada fileira com um sprite
 * diferente) se desloca lateralmente em bloco e desce um degrau sempre
 * que toca a borda da tela — o padrão de movimento oscilante clássico.
 * O jogador controla o canhão com 2 botões (esquerda/direita) e atira
 * com um terceiro botão. Vários tiros do jogador e dos inimigos convivem
 * na tela ao mesmo tempo, cada um gerenciado num array pequeno de
 * structs. De tempos em tempos um inimigo vivo (sempre o mais baixo de
 * uma coluna) atira de volta. O buzzer toca efeitos diferentes para
 * tiro, explosão e game over. Placar e recorde (EEPROM) na faixa
 * amarela; campo de jogo na faixa azul.
 *
 * O maior desafio de memória: com o Uno tendo só 2KB de SRAM (e o
 * próprio buffer da OLED já consome 1KB deles), a grade de 18 inimigos
 * é guardada como um único bitmask (um bit por inimigo) em vez de um
 * array de structs com posição — a posição de cada inimigo é sempre
 * calculada a partir de uma única posição-base da grade + índice de
 * linha/coluna. Os sprites ficam em PROGMEM (memória Flash), que sobra
 * bastante (32KB), sem gastar RAM.
 *
 * Pinout:
 *   Botão ESQUERDA -> D2
 *   Botão DIREITA  -> D3
 *   Botão TIRO     -> D4
 *   Buzzer         -> D8
 *   OLED SDA       -> A4 (I2C)
 *   OLED SCL       -> A5 (I2C)
 *
 * Bibliotecas:
 *   Adafruit SSD1306
 *   Adafruit GFX
 *   EEPROM (nativa do Arduino)
 */

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <EEPROM.h>

// ---------- Pinout ----------
#define PIN_BTN_ESQUERDA 9
#define PIN_BTN_DIREITA  13
#define PIN_BTN_TIRO     11
#define PIN_BUZZER       7

// ---------- OLED ----------
#define SCREEN_WIDTH  128
#define SCREEN_HEIGHT 64
#define OLED_RESET    -1
#define OLED_ADDR     0x3C

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// ---------- Layout (faixa amarela = placar, faixa azul = campo) ----------
const uint8_t HEADER_DIVIDER_Y = 15;
const uint8_t CAMPO_Y0         = 16;
const uint8_t CAMPO_Y1         = SCREEN_HEIGHT - 1; // 63

// ---------- Sprites 8x8 (PROGMEM: fica na Flash, não gasta SRAM) ----------
const uint8_t SPRITE_W = 8;
const uint8_t SPRITE_H = 8;

const uint8_t spriteInimigoTopo[SPRITE_H] PROGMEM = {
  0b00100100,
  0b00100100,
  0b01111110,
  0b11011011,
  0b11111111,
  0b10111101,
  0b10100101,
  0b00011000,
};

const uint8_t spriteInimigoMeio[SPRITE_H] PROGMEM = {
  0b00011000,
  0b00111100,
  0b01111110,
  0b11011011,
  0b11111111,
  0b00100100,
  0b01011010,
  0b10100101,
};

const uint8_t spriteInimigoBase[SPRITE_H] PROGMEM = {
  0b00111100,
  0b01111110,
  0b11011011,
  0b11111111,
  0b01111110,
  0b00100100,
  0b01011010,
  0b10100101,
};

const uint8_t spriteJogador[SPRITE_H] PROGMEM = {
  0b00011000,
  0b00011000,
  0b00111100,
  0b01111110,
  0b11111111,
  0b11111111,
  0b11111111,
  0b11111111,
};

const uint8_t *SPRITES_INIMIGO[3] = { spriteInimigoTopo, spriteInimigoMeio, spriteInimigoBase };

// ---------- Grade de inimigos ----------
// Em vez de um array de structs com posição por inimigo (caro em SRAM),
// a grade inteira é representada por UMA posição-base (inimigoBaseX/Y) +
// um bitmask de "quem ainda está vivo". A posição de cada inimigo é
// sempre recalculada a partir do índice de linha/coluna.
const uint8_t ENEMY_COLS = 6;
const uint8_t ENEMY_ROWS = 3;
const uint8_t TOTAL_INIMIGOS = ENEMY_COLS * ENEMY_ROWS; // 18 (cabe num uint32_t)
const uint8_t ENEMY_SPACING_X = 14;
const uint8_t ENEMY_SPACING_Y = 10;
const uint8_t ENEMY_MARGIN_X  = 4;  // margem antes de inverter direção
const uint8_t ENEMY_STEP_Y    = 6;  // desce esse tanto ao bater na borda
const uint8_t ENEMY_MOVE_STEP_X = 2;
const int16_t ENEMY_GRID_WIDTH = (ENEMY_COLS - 1) * ENEMY_SPACING_X + SPRITE_W;
const int16_t ENEMY_BASE_X_INICIAL = (SCREEN_WIDTH - ENEMY_GRID_WIDTH) / 2;
const int16_t ENEMY_BASE_Y_INICIAL = CAMPO_Y0 + 2;

uint32_t inimigosVivos;      // bit (linha*ENEMY_COLS+coluna) = 1 -> inimigo vivo
int16_t  inimigoBaseX;
int16_t  inimigoBaseY;
int8_t   inimigoDirecao = 1; // +1 direita, -1 esquerda

// Pontos por fileira (fileira 0 = topo = mais rara/valiosa, como no clássico)
const uint16_t PONTOS_POR_FILEIRA[ENEMY_ROWS] = { 30, 20, 10 };

// ---------- Velocidade da grade (acelera conforme mata inimigos e por onda) ----------
const unsigned long INTERVALO_INIMIGOS_INICIAL_MS = 600;
const unsigned long INTERVALO_INIMIGOS_MINIMO_MS  = 110;
const unsigned long ONDA_ACELERACAO_MS            = 60; // reduz o intervalo base a cada onda nova
unsigned long intervaloInimigosAtual = INTERVALO_INIMIGOS_INICIAL_MS;
unsigned long proximoMovimentoInimigos = 0;

// ---------- Tiro dos inimigos ----------
const uint8_t MAX_TIROS_INIMIGOS = 3;
struct Tiro {
  bool ativo;
  int16_t x, y;
};
Tiro tirosInimigos[MAX_TIROS_INIMIGOS];
const unsigned long INTERVALO_TIRO_INIMIGO_MIN_MS = 700;
const unsigned long INTERVALO_TIRO_INIMIGO_MAX_MS = 1800;
unsigned long proximoTiroInimigo = 0;
const int8_t ENEMY_SHOT_SPEED = 2;

// ---------- Jogador ----------
const uint8_t PLAYER_WIDTH  = SPRITE_W;
const uint8_t PLAYER_HEIGHT = SPRITE_H;
const uint8_t PLAYER_Y = CAMPO_Y1 - PLAYER_HEIGHT - 1;
const int8_t  PLAYER_SPEED = 2;
int16_t playerX = (SCREEN_WIDTH - PLAYER_WIDTH) / 2;

// Linha de invasão: se a fileira mais baixa de inimigos passar daqui, é game over
const uint8_t LINHA_INVASAO = PLAYER_Y - 2;

// ---------- Tiro do jogador ----------
const uint8_t MAX_TIROS_JOGADOR = 2;
Tiro tirosJogador[MAX_TIROS_JOGADOR];
const int8_t PLAYER_SHOT_SPEED = 3;

// ---------- Botão de tiro (debounce por software, mesmo padrão do Snake) ----------
const unsigned long DEBOUNCE_MS = 40;
struct Botao {
  uint8_t pino;
  bool estadoEstavel;
  bool ultimaLeitura;
  unsigned long ultimaMudanca;
};
Botao botaoTiro = { PIN_BTN_TIRO, HIGH, HIGH, 0 };

// ---------- Placar, vidas e ondas ----------
uint16_t pontuacao = 0;
uint16_t recorde = 0;
const int ENDERECO_EEPROM_RECORDE = 0;
uint8_t vidas = 3;
uint8_t onda = 1;
const uint8_t VIDAS_INICIAIS = 3;

// ---------- Estado do jogo ----------
enum Estado : uint8_t { TELA_INICIAL, JOGANDO, GAME_OVER };
Estado estado = TELA_INICIAL;
bool venceuOnda = false; // usado só pra decidir a mensagem no game over (não usado no momento)

// ---------- Timing não-bloqueante ----------
unsigned long proximoFrame = 0;
const unsigned long FRAME_MS = 30;

void setup() {
  pinMode(PIN_BTN_ESQUERDA, INPUT_PULLUP);
  pinMode(PIN_BTN_DIREITA, INPUT_PULLUP);
  pinMode(PIN_BTN_TIRO, INPUT_PULLUP);
  pinMode(PIN_BUZZER, OUTPUT);

  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    // Trava aqui se a OLED não inicializar — sinal de erro de fiação/endereço I2C
    for (;;) {}
  }
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);

  randomSeed(analogRead(A0)); // pino analógico flutuante (não usado) garante uma seed variável

  EEPROM.get(ENDERECO_EEPROM_RECORDE, recorde);
  if (recorde == 0xFFFF) recorde = 0; // EEPROM "virgem" lê 0xFF em todos os bytes

  desenharTelaInicial();
}

void loop() {
  lerBotaoTiro();

  switch (estado) {
    case TELA_INICIAL:
      if (algumBotaoPressionado()) {
        iniciarJogo();
      }
      break;

    case JOGANDO: {
      unsigned long agora = millis();
      if (agora < proximoFrame) return;
      proximoFrame = agora + FRAME_MS;

      moverJogador();
      atualizarTirosJogador();
      atualizarTirosInimigos();
      moverGradeInimigos(agora);
      dispararInimigoSeNaHora(agora);
      verificarColisoes();
      verificarFimDeJogo();

      if (estado == JOGANDO) {
        desenharTela();
      }
      break;
    }

    case GAME_OVER:
      if (algumBotaoPressionado()) {
        desenharTelaInicial();
        estado = TELA_INICIAL;
      }
      break;
  }
}

// =====================================================================
// Botões
// =====================================================================
void lerBotaoTiro() {
  bool leitura = digitalRead(botaoTiro.pino);
  if (leitura != botaoTiro.ultimaLeitura) {
    botaoTiro.ultimaMudanca = millis();
  }
  if ((millis() - botaoTiro.ultimaMudanca) > DEBOUNCE_MS) {
    if (leitura != botaoTiro.estadoEstavel) {
      botaoTiro.estadoEstavel = leitura;
      if (botaoTiro.estadoEstavel == LOW && estado == JOGANDO) { // borda de descida = pressionado
        dispararJogador();
      }
    }
  }
  botaoTiro.ultimaLeitura = leitura;
}

bool algumBotaoPressionado() {
  return digitalRead(PIN_BTN_ESQUERDA) == LOW ||
         digitalRead(PIN_BTN_DIREITA) == LOW ||
         digitalRead(PIN_BTN_TIRO) == LOW;
}

// Movimento lateral contínuo: enquanto o botão estiver pressionado, o
// canhão anda PLAYER_SPEED px a cada frame (sem precisar de debounce,
// já que aqui o interesse é o nível do sinal, não a borda).
void moverJogador() {
  if (digitalRead(PIN_BTN_ESQUERDA) == LOW) {
    playerX -= PLAYER_SPEED;
  }
  if (digitalRead(PIN_BTN_DIREITA) == LOW) {
    playerX += PLAYER_SPEED;
  }
  playerX = constrain(playerX, 0, SCREEN_WIDTH - PLAYER_WIDTH);
}

// =====================================================================
// Ciclo de vida do jogo
// =====================================================================
void iniciarJogo() {
  pontuacao = 0;
  vidas = VIDAS_INICIAIS;
  onda = 1;
  playerX = (SCREEN_WIDTH - PLAYER_WIDTH) / 2;

  for (uint8_t i = 0; i < MAX_TIROS_JOGADOR; i++) tirosJogador[i].ativo = false;
  for (uint8_t i = 0; i < MAX_TIROS_INIMIGOS; i++) tirosInimigos[i].ativo = false;

  iniciarOnda();

  estado = JOGANDO;
  proximoFrame = millis();
  desenharTela();
}

// Reseta a grade de inimigos pra uma nova onda (todos vivos de novo),
// um pouco mais rápida que a onda anterior, mantendo placar e vidas.
void iniciarOnda() {
  inimigosVivos = (TOTAL_INIMIGOS >= 32) ? 0xFFFFFFFF : ((1UL << TOTAL_INIMIGOS) - 1);
  inimigoBaseX = ENEMY_BASE_X_INICIAL;
  inimigoBaseY = ENEMY_BASE_Y_INICIAL;
  inimigoDirecao = 1;

  unsigned long intervaloBaseOnda = INTERVALO_INIMIGOS_INICIAL_MS - (unsigned long)(onda - 1) * ONDA_ACELERACAO_MS;
  if (intervaloBaseOnda < INTERVALO_INIMIGOS_MINIMO_MS) intervaloBaseOnda = INTERVALO_INIMIGOS_MINIMO_MS;
  intervaloInimigosAtual = intervaloBaseOnda;
  proximoMovimentoInimigos = millis() + intervaloInimigosAtual;

  proximoTiroInimigo = millis() + random(INTERVALO_TIRO_INIMIGO_MIN_MS, INTERVALO_TIRO_INIMIGO_MAX_MS);

  for (uint8_t i = 0; i < MAX_TIROS_INIMIGOS; i++) tirosInimigos[i].ativo = false;
}

void verificarFimDeJogo() {
  // Todos os inimigos da onda morreram: sobe de onda (grade renasce mais rápida)
  if (inimigosVivos == 0) {
    onda++;
    tocarBeepOndaCompleta();
    iniciarOnda();
    return;
  }

  // Grade de inimigos chegou perto demais do jogador -> invasão, fim de jogo
  int16_t yFileiraMaisBaixa = inimigoBaseY + (ENEMY_ROWS - 1) * ENEMY_SPACING_Y + SPRITE_H;
  if (yFileiraMaisBaixa >= LINHA_INVASAO) {
    finalizarJogo();
  }
}

void finalizarJogo() {
  tocarBeepGameOver();

  bool novoRecorde = false;
  if (pontuacao > recorde) {
    recorde = pontuacao;
    EEPROM.put(ENDERECO_EEPROM_RECORDE, recorde);
    novoRecorde = true;
  }

  estado = GAME_OVER;
  desenharTelaGameOver(novoRecorde);
}

// =====================================================================
// Grade de inimigos
// =====================================================================
bool inimigoVivo(uint8_t linha, uint8_t coluna) {
  uint8_t indice = linha * ENEMY_COLS + coluna;
  return (inimigosVivos >> indice) & 0x01;
}

void matarInimigo(uint8_t linha, uint8_t coluna) {
  uint8_t indice = linha * ENEMY_COLS + coluna;
  inimigosVivos &= ~((uint32_t)1 << indice);
}

uint8_t contarInimigosVivos() {
  uint8_t contagem = 0;
  for (uint8_t i = 0; i < TOTAL_INIMIGOS; i++) {
    if ((inimigosVivos >> i) & 0x01) contagem++;
  }
  return contagem;
}

// Desloca a grade inteira lateralmente; ao encostar numa borda, inverte
// a direção e desce um degrau (o padrão oscilante clássico). A borda
// esquerda/direita é calculada a partir da coluna viva mais externa (e
// não da largura total da grade), pra continuar precisa conforme os
// inimigos das pontas vão morrendo.
void moverGradeInimigos(unsigned long agora) {
  if (agora < proximoMovimentoInimigos) return;

  uint8_t vivosRestantes = contarInimigosVivos();
  intervaloInimigosAtual = map(vivosRestantes, TOTAL_INIMIGOS, 1,
                                intervaloInimigosAtual, INTERVALO_INIMIGOS_MINIMO_MS);
  if (intervaloInimigosAtual < INTERVALO_INIMIGOS_MINIMO_MS) {
    intervaloInimigosAtual = INTERVALO_INIMIGOS_MINIMO_MS;
  }
  proximoMovimentoInimigos = agora + intervaloInimigosAtual;

  int8_t colunaMin = -1, colunaMax = -1;
  for (int8_t c = 0; c < ENEMY_COLS; c++) {
    for (uint8_t l = 0; l < ENEMY_ROWS; l++) {
      if (inimigoVivo(l, c)) {
        if (colunaMin == -1) colunaMin = c;
        colunaMax = c;
      }
    }
  }
  if (colunaMin == -1) return; // sem inimigos vivos (tratado em verificarFimDeJogo)

  int16_t bordaEsquerda = inimigoBaseX + colunaMin * ENEMY_SPACING_X;
  int16_t bordaDireita  = inimigoBaseX + colunaMax * ENEMY_SPACING_X + SPRITE_W;

  bool bateuNaBorda =
      (inimigoDirecao > 0 && bordaDireita + ENEMY_MOVE_STEP_X >= SCREEN_WIDTH - ENEMY_MARGIN_X) ||
      (inimigoDirecao < 0 && bordaEsquerda - ENEMY_MOVE_STEP_X <= ENEMY_MARGIN_X);

  if (bateuNaBorda) {
    inimigoDirecao = -inimigoDirecao;
    inimigoBaseY += ENEMY_STEP_Y;
    tocarBeepPassoInimigo();
  } else {
    inimigoBaseX += inimigoDirecao * ENEMY_MOVE_STEP_X;
  }
}

// Escolhe uma coluna aleatória que ainda tenha algum inimigo vivo e faz
// o inimigo mais baixo dela atirar (mesma regra do jogo original: só o
// inimigo na "linha de frente" de cada coluna consegue atirar).
void dispararInimigoSeNaHora(unsigned long agora) {
  if (agora < proximoTiroInimigo) return;
  proximoTiroInimigo = agora + random(INTERVALO_TIRO_INIMIGO_MIN_MS, INTERVALO_TIRO_INIMIGO_MAX_MS);

  uint8_t colunasVivas[ENEMY_COLS];
  uint8_t totalColunasVivas = 0;
  for (uint8_t c = 0; c < ENEMY_COLS; c++) {
    for (uint8_t l = 0; l < ENEMY_ROWS; l++) {
      if (inimigoVivo(l, c)) {
        colunasVivas[totalColunasVivas++] = c;
        break;
      }
    }
  }
  if (totalColunasVivas == 0) return;

  uint8_t colunaEscolhida = colunasVivas[random(0, totalColunasVivas)];
  int8_t linhaMaisBaixa = -1;
  for (int8_t l = ENEMY_ROWS - 1; l >= 0; l--) {
    if (inimigoVivo(l, colunaEscolhida)) {
      linhaMaisBaixa = l;
      break;
    }
  }
  if (linhaMaisBaixa == -1) return;

  for (uint8_t i = 0; i < MAX_TIROS_INIMIGOS; i++) {
    if (!tirosInimigos[i].ativo) {
      tirosInimigos[i].ativo = true;
      tirosInimigos[i].x = inimigoBaseX + colunaEscolhida * ENEMY_SPACING_X + SPRITE_W / 2;
      tirosInimigos[i].y = inimigoBaseY + linhaMaisBaixa * ENEMY_SPACING_Y + SPRITE_H;
      break;
    }
  }
}

// =====================================================================
// Tiros
// =====================================================================
void dispararJogador() {
  for (uint8_t i = 0; i < MAX_TIROS_JOGADOR; i++) {
    if (!tirosJogador[i].ativo) {
      tirosJogador[i].ativo = true;
      tirosJogador[i].x = playerX + PLAYER_WIDTH / 2;
      tirosJogador[i].y = PLAYER_Y - 3;
      tocarBeepTiro();
      break;
    }
  }
}

void atualizarTirosJogador() {
  for (uint8_t i = 0; i < MAX_TIROS_JOGADOR; i++) {
    if (!tirosJogador[i].ativo) continue;
    tirosJogador[i].y -= PLAYER_SHOT_SPEED;
    if (tirosJogador[i].y < CAMPO_Y0) {
      tirosJogador[i].ativo = false;
    }
  }
}

void atualizarTirosInimigos() {
  for (uint8_t i = 0; i < MAX_TIROS_INIMIGOS; i++) {
    if (!tirosInimigos[i].ativo) continue;
    tirosInimigos[i].y += ENEMY_SHOT_SPEED;
    if (tirosInimigos[i].y > CAMPO_Y1) {
      tirosInimigos[i].ativo = false;
    }
  }
}

// =====================================================================
// Colisões
// =====================================================================
void verificarColisoes() {
  // Tiros do jogador x inimigos (varre todos os inimigos vivos; a grade
  // é pequena o bastante — no máximo 18 — pra isso ser barato).
  for (uint8_t i = 0; i < MAX_TIROS_JOGADOR; i++) {
    if (!tirosJogador[i].ativo) continue;

    for (uint8_t l = 0; l < ENEMY_ROWS && tirosJogador[i].ativo; l++) {
      for (uint8_t c = 0; c < ENEMY_COLS; c++) {
        if (!inimigoVivo(l, c)) continue;

        int16_t ex = inimigoBaseX + c * ENEMY_SPACING_X;
        int16_t ey = inimigoBaseY + l * ENEMY_SPACING_Y;
        if (tirosJogador[i].x >= ex && tirosJogador[i].x <= ex + SPRITE_W &&
            tirosJogador[i].y >= ey && tirosJogador[i].y <= ey + SPRITE_H) {
          matarInimigo(l, c);
          tirosJogador[i].ativo = false;
          pontuacao += PONTOS_POR_FILEIRA[l];
          tocarBeepExplosao();
          break;
        }
      }
    }
  }

  // Tiros dos inimigos x jogador
  for (uint8_t i = 0; i < MAX_TIROS_INIMIGOS; i++) {
    if (!tirosInimigos[i].ativo) continue;

    if (tirosInimigos[i].x >= playerX && tirosInimigos[i].x <= playerX + PLAYER_WIDTH &&
        tirosInimigos[i].y >= PLAYER_Y && tirosInimigos[i].y <= PLAYER_Y + PLAYER_HEIGHT) {
      tirosInimigos[i].ativo = false;
      tocarBeepExplosao();

      if (vidas > 0) vidas--;
      if (vidas == 0) {
        finalizarJogo();
        return;
      }
    }
  }
}

// =====================================================================
// Som
// =====================================================================
void tocarBeepTiro()         { tone(PIN_BUZZER, 1400, 30); }
void tocarBeepExplosao()     { tone(PIN_BUZZER, 250, 90); }
void tocarBeepPassoInimigo() { tone(PIN_BUZZER, 600, 15); }
void tocarBeepOndaCompleta() { tone(PIN_BUZZER, 1800, 150); }
void tocarBeepGameOver()     { tone(PIN_BUZZER, 150, 400); }

// =====================================================================
// Desenho
// =====================================================================
void desenharPlacar() {
  display.setTextSize(1);

  char textoPontos[10];
  char textoVidas[10];
  sprintf(textoPontos, "SCORE:%u", pontuacao);
  sprintf(textoVidas, "VIDAS:%u", vidas);

  display.setCursor(4, 4);
  display.print(textoPontos);

  int16_t x1, y1;
  uint16_t w, h;
  display.getTextBounds(textoVidas, 0, 0, &x1, &y1, &w, &h);
  display.setCursor(SCREEN_WIDTH - w - 4, 4);
  display.print(textoVidas);
}

void desenharTela() {
  display.clearDisplay();

  desenharPlacar();
  display.drawFastHLine(0, HEADER_DIVIDER_Y, SCREEN_WIDTH, SSD1306_WHITE);

  // Grade de inimigos
  for (uint8_t l = 0; l < ENEMY_ROWS; l++) {
    for (uint8_t c = 0; c < ENEMY_COLS; c++) {
      if (!inimigoVivo(l, c)) continue;
      int16_t ex = inimigoBaseX + c * ENEMY_SPACING_X;
      int16_t ey = inimigoBaseY + l * ENEMY_SPACING_Y;
      display.drawBitmap(ex, ey, SPRITES_INIMIGO[l], SPRITE_W, SPRITE_H, SSD1306_WHITE);
    }
  }

  // Jogador
  display.drawBitmap(playerX, PLAYER_Y, spriteJogador, SPRITE_W, SPRITE_H, SSD1306_WHITE);

  // Tiros
  for (uint8_t i = 0; i < MAX_TIROS_JOGADOR; i++) {
    if (tirosJogador[i].ativo) {
      display.drawFastVLine(tirosJogador[i].x, tirosJogador[i].y, 3, SSD1306_WHITE);
    }
  }
  for (uint8_t i = 0; i < MAX_TIROS_INIMIGOS; i++) {
    if (tirosInimigos[i].ativo) {
      display.fillRect(tirosInimigos[i].x, tirosInimigos[i].y, 2, 2, SSD1306_WHITE);
    }
  }

  display.display();
}

void desenharTelaInicial() {
  display.clearDisplay();

  display.setTextSize(2);
  const char *titulo = "INVADERS";
  int16_t x1, y1;
  uint16_t w, h;
  display.getTextBounds(titulo, 0, 0, &x1, &y1, &w, &h);
  display.setTextSize(w > SCREEN_WIDTH ? 1 : 2);
  display.getTextBounds(titulo, 0, 0, &x1, &y1, &w, &h);
  display.setCursor((SCREEN_WIDTH - w) / 2, 0);
  display.print(titulo);

  display.drawFastHLine(0, HEADER_DIVIDER_Y, SCREEN_WIDTH, SSD1306_WHITE);

  display.setTextSize(1);
  const char *msg = "Aperte um botao";
  display.getTextBounds(msg, 0, 0, &x1, &y1, &w, &h);
  display.setCursor((SCREEN_WIDTH - w) / 2, 34);
  display.print(msg);

  char textoRecorde[16];
  sprintf(textoRecorde, "Recorde: %u", recorde);
  display.getTextBounds(textoRecorde, 0, 0, &x1, &y1, &w, &h);
  display.setCursor((SCREEN_WIDTH - w) / 2, 48);
  display.print(textoRecorde);

  display.display();
}

void desenharTelaGameOver(bool novoRecorde) {
  display.clearDisplay();

  display.setTextSize(1);
  const char *titulo = "GAME OVER";
  int16_t x1, y1;
  uint16_t w, h;
  display.getTextBounds(titulo, 0, 0, &x1, &y1, &w, &h);
  display.setCursor((SCREEN_WIDTH - w) / 2, 4);
  display.print(titulo);

  display.drawFastHLine(0, HEADER_DIVIDER_Y, SCREEN_WIDTH, SSD1306_WHITE);

  char textoPontos[16];
  sprintf(textoPontos, "Pontos: %u", pontuacao);
  display.getTextBounds(textoPontos, 0, 0, &x1, &y1, &w, &h);
  display.setCursor((SCREEN_WIDTH - w) / 2, 26);
  display.print(textoPontos);

  char textoOnda[12];
  sprintf(textoOnda, "Onda: %u", onda);
  display.getTextBounds(textoOnda, 0, 0, &x1, &y1, &w, &h);
  display.setCursor((SCREEN_WIDTH - w) / 2, 38);
  display.print(textoOnda);

  const char *linha3 = novoRecorde ? "NOVO RECORDE!" : "Aperte um botao";
  display.getTextBounds(linha3, 0, 0, &x1, &y1, &w, &h);
  display.setCursor((SCREEN_WIDTH - w) / 2, 52);
  display.print(linha3);

  display.display();
}
