//
// file: TPS2.cpp
// Arduino STM32用 PS/2インタフェース by たま吉さん
// 作成日 2017/01/31
// 修正日 2017/02/04, LED制御のための修正
// 修正日 2017/02/04, rcev()関数の追加
// 修正日 2017/02/05, send(),setPriority()関数の追加
//

#include <TPS2.h>
//#include <io.h>
//#include <libmaple/gpio.h>
//#include <libmaple/timer.h>
//#include <libmaple/exti.h>

volatile uint8_t _clkPin; // CLKピン
volatile static uint8_t _datPin; // DATピン
volatile static uint8_t _clkDir; // CLK入出力方向
volatile static uint8_t _datDir; // CLK入出力方向
volatile static uint8_t _err;    // 直前のAPIエラー

static uint8_t  _queue[QUEUESIZE];  // 受信バッファ
volatile static uint16_t _q_top; // バッファ取出し位置
volatile static uint16_t _q_btm; // バッファ挿入位置

volatile static uint8_t _flg_rs;   // 送受信モード 0:受信 1:送信

volatile static uint8_t _sendState = 0;   // ビット処理状態
volatile static uint8_t _sendData  = 0;   // 送信データ
volatile static uint8_t _sendParity = 1;  // パリティビット  
typedef int nvic_irq_num;
typedef int exti_num;
volatile static nvic_irq_num  _irq_num;   // 割り込みベクター番号

#ifdef PS2_POLLING
bool TPS2::_intr_ena;
#endif

// CLK変化割り込み許可
void TPS2::enableInterrupts() {
#ifdef PS2_POLLING
  _intr_ena = true;
#else
  interrupts();
#endif
}

// CLK変化割り込み禁止
void TPS2::disableInterrupts() {
#ifdef PS2_POLLING
  _intr_ena = false;
#else
  noInterrupts();
#endif
}

// 割り込み優先レベルの設定
void TPS2::setPriority(uint8_t n) {
}

// ポート番号の設定
// 引数
//   clk : PS/2 CLK
//   dat : PS/2 DATA
//
void TPS2::setPort(uint8_t clk, uint8_t dat) { // GPIOピンの設定
  _clkPin = clk; 
  _datPin = dat;
}

//
// エラー参照
// ※割り込みハンドラで発生したエラーチェック用
// 戻り値  0:正常終了 0以外:異常終了(エラーコード)
//
uint8_t TPS2::error() {
  return _err;
};

//
// 利用開始
// ※ 開始直後のPS/2ラインは停止状態です(割り込みも禁止状態)。
//    利用開始後、上位レベルのライブラリにて接続デバイスの初期化に通信と割り込みを
//    有効にして下さい。
//
// 引数
//   clk : PS/2 CLK
//   dat : PS/2 DATA
//

void TPS2::begin(uint8_t clk, uint8_t dat) {
  setPort(clk,dat);
  _irq_num = _clkPin;//exti2irqNum((exti_num)PIN_MAP[_clkPin].gpio_bit);
  
  // バスを停止状態に設定
  clkSet_Out();
  datSet_Out();
  mode_stop(); // PS/2ライン状態は停止状態
  _err = 0;

  // キー入力バッファの初期化
  clear_queue();  
  
  // 割り込みハンドラの登録
#ifndef PS2_POLLING
  attachInterrupt(_clkPin, clkPinHandle, FALLING);
#endif
  disableInterrupts();
}

// 利用終了
void TPS2::end() {
  disableInterrupts();
#ifndef PS2_POLLING
  detachInterrupt(_clkPin);
#endif
}

#ifdef ESP32
// no pull-ups on the Bauern-Box
#define PS2_OUTPUT	OUTPUT
#else
#define PS2_OUTPUT	OUTPUT_OPEN_DRAIN
#endif

// CLKを出力モードに設定
void TPS2::clkSet_Out() {
  pinMode(_clkPin, PS2_OUTPUT);
  _clkDir = D_OUT;  
}

// CLKを入力モードに設定
void TPS2::clkSet_In() {
  pinMode(_clkPin, INPUT_PULLUP);
  _clkDir = D_IN;
}

// DATを出力モードに設定
void TPS2::datSet_Out() {
  pinMode(_datPin, PS2_OUTPUT);
  _datDir = D_OUT;    
}

// DATを入力モードに設定
void TPS2::datSet_In() {
  pinMode(_datPin, INPUT_PULLUP);
  _datDir = D_IN;
}

// CLKから出力
void TPS2::Clk_Out(uint8_t val) {
  if (_clkDir == D_IN)
    clkSet_Out();    
  digitalWrite(_clkPin, val);
}

// CLKから入力
uint8_t ICACHE_RAM_ATTR TPS2::Clk_In() {
  if (_clkDir == D_OUT)
    clkSet_In();
  return digitalRead(_clkPin) ? HIGH : LOW;
}

// DATから出力
void TPS2::Dat_Out(uint8_t val) {
  if (_datDir == D_IN)
    datSet_Out();    
  digitalWrite(_datPin, val);
}

// データから入力
uint8_t ICACHE_RAM_ATTR TPS2::Dat_In() {
  if (_datDir == D_OUT)
    datSet_In();    
  return digitalRead(_datPin) ? HIGH : LOW;
}

// アイドル状態に設定
void TPS2::mode_idole(uint8_t dir) {
  if (dir == D_IN) {
    // 入力状態で開放(PINはプルアップ抵抗によりHIGHとなる)
    clkSet_In();
    datSet_In();
  } else {
    Clk_Out(HIGH);
    Dat_Out(HIGH);    
  }
}

// 通信禁止
void TPS2::mode_stop() {
  Clk_Out(LOW);
  Dat_Out(HIGH);    
  delayMicroseconds(100);
}

// ホスト送信モード
void TPS2::mode_send() {
  Dat_Out(LOW);
  Clk_Out(HIGH); 
}

// CLKの状態変化待ち
uint8_t TPS2::wait_Clk(uint8_t val, uint32_t tm) {
  uint8_t err = 1;
  while (1) {
    if (Clk_In() == val) {
      err = 0;
      break;
    }
    tm--;
    if (!tm)
      break;
    delayMicroseconds(1);
  }
  return err;
}

// DATの状態待ち
uint8_t TPS2::wait_Dat(uint8_t val, uint32_t tm) {
  uint8_t err = 1;
  while (1) {
    if (Dat_In() == val) {
      err = 0;
      break;
    }
    tm--;
    if (!tm)
      break;
    delayMicroseconds(1);
  }
  _err = err;
  return err;
}

// データ送信
uint8_t TPS2::hostSend(uint8_t data) {
  uint8_t err = 0;    // エラーコード
  bool parity = true; // パリティ

  // 送信準備
  disableInterrupts();    // 割り込み禁止
  mode_stop();            // バスを送信禁止状態に設定
  mode_send();            // バスをホストからの[送信要求]に設定
 
  if (wait_Clk(LOW, 10000)) { err = 1; goto ERROR; } // デバイスがCLKラインをLOWにするのを待つ(最大10msec)   

  // データ8ビット送信
  for (uint8_t i = 0; i < 8; i++) {
    // 1ビット送信
    delayMicroseconds(15);
    if (bitRead(data, i)) {
      parity = !parity;
      Dat_Out(HIGH);
    } else {
      Dat_Out(LOW);
    }
    // デバイスがクロックをHIGHにするまで待つ
    if (wait_Clk(HIGH, 50)) {
      err = 2;
      goto ERROR;
    }
    // デバイスがクロックをLOWにするまで待つ
    if (wait_Clk(LOW, 50)) {
      err = 3;
      goto ERROR;
    }
  }

  // パリティビットの送信
  delayMicroseconds(15);
  if (parity)
    Dat_Out(HIGH);
  else
    Dat_Out(LOW);
  // デバイスがクロックをHIGHにするまで待つ
  if (wait_Clk(HIGH, 50)) {
    err = 4;
    goto ERROR;
  }
  // デバイスがクロックをLOWにするまで待つ
  if (wait_Clk(LOW, 50)) {
    err = 6;
    goto ERROR;
  }

  // STOPビットの送信
  delayMicroseconds(15);
  Dat_Out(HIGH);

  // デバイスからのACK確認
  if (wait_Dat(LOW, 50)) {
    err = 7;
    goto ERROR;
  }
  if (wait_Clk(LOW, 50)) {
    err = 8;
    goto ERROR;
  }

  // バス開放待ち
  if (wait_Dat(HIGH, 50)) {
    err = 9;
    goto ERROR;
  }
  if (wait_Clk(HIGH, 50)) {
    err = 10;
    goto ERROR;
  }

ERROR:                 // 終了処理
  mode_idole(D_IN);    // バスをアイドル状態にする
  enableInterrupts();  // 割り込み許可
  return err;
}

//
// データ受信(キューからの取り出し、割り込み経由)
//
uint8_t TPS2::rcev(uint8_t* rcv) {
  uint8_t err = 1;
  uint16_t tm = 1500;

  do {
    if (available()) {
      *rcv = dequeue();
      err = 0;
      break;
    }
    delayMicroseconds(1);
    tm--;
  } while (tm);
  return err;
}

// データ受信
uint8_t TPS2::HostRcev(uint8_t *rcv) {
  uint8_t data = 0;
  bool parity = true;
  uint8_t err = 0;

  // バス準備
  disableInterrupts();    // 割り込み禁止

  // STARTビットの受信
  if (wait_Clk(LOW, 1000)) {
    err = 1;
    goto ERROR;
  }
  // STARTは常に0のはず
  if (wait_Dat(LOW, 1)) {
    err = 2;
    goto ERROR;
  }
  if (wait_Clk(HIGH, 50)) {
    err = 3;
    goto ERROR;
  }

  //8ビット分データの受信ループ
  for (uint8_t i = 0; i < 8; i++) {
    // CLKがLOWになるのを待つ
    if (wait_Clk(LOW, 50)) {
      err = 4;
      goto ERROR;
    }
    // 1ビット分の取得
    if (Dat_In()) {
      parity = !parity;
      data |= (1 << i);
    }
    // CLKがHIGHになるのを待つ
    if (wait_Clk(HIGH, 50)) {
      err = 5;
      goto ERROR;
    }
  }

  //PARITYビットの取得
  // CLKがLOWになるのを待つ
  if (wait_Clk(LOW, 50)) {
    err = 6;
    goto ERROR;
  }
  // パリティチェック
  if (Dat_In() != parity) {
    err = 7;
    goto ERROR;
  }
  if (wait_Clk(HIGH, 50)) {
    err = 8;
    goto ERROR;
  }

  // ストップビットの取得
  if (wait_Clk(LOW, 50)) {
    err = 9;
    goto ERROR;
  }
  // STOPは常に1のはず
  if (wait_Dat(HIGH, 1)) {
    err = 10;
    goto ERROR;
  }
  if (wait_Clk(HIGH, 50)) {
    err = 11;
    goto ERROR;
  }

ERROR:

  enableInterrupts();
  *rcv = data;
  return err;
}

// 応答受信
uint8_t TPS2::response(uint8_t *rcv) {
  uint8_t tm = 250;
  uint8_t err = 0;
  while (1) {
    err = HostRcev(rcv);
    if (!err)
      break;
    tm--;
    if (!tm)
      break;
  }
  return err;
}

// 割り込みハンドラ経由データ送信
uint8_t TPS2::send(uint8_t data) {
  // 送信データセット
  _sendData = data;       // 送信データセット
  _flg_rs = 1;
  _sendState = 0;

  // 送信準備
  disableInterrupts();    // 割り込み禁止
  mode_stop();            // バスを送信禁止状態に設定
  mode_send();            // バスをホストからの[送信要求]に設定
  clkSet_In();
  
  enableInterrupts();      // 割り込み許可

  while(_flg_rs);

  disableInterrupts();    // 割り込み禁止
  mode_idole(D_IN);        // バスをアイドル状態にする(2017/02/4)
  enableInterrupts();      // 割り込み許可
  return _err;  
}

//
// 割り込みハンドラサブルーチン
// データ送信用
//
void ICACHE_RAM_ATTR TPS2::clkPinHandleSend() {
  if (Clk_In()) {
    goto END;
  }

  _sendState++;
  if (_sendState >= 1 && _sendState <= 8) {
    // データの送信
    delayMicroseconds(15);
    if (_sendData & 1) {
      Dat_Out(HIGH);
      _sendParity++;
    } else {
      Dat_Out(LOW);
    }
    _sendData >>= 1;
    goto END;
  } else if (_sendState == 9) {
    // パリティ送信
    delayMicroseconds(15);
    Dat_Out(_sendParity & 1);
    goto END;
  } else if (_sendState == 10) {
    // ストップビット送信
    delayMicroseconds(15);
    Dat_Out(HIGH); 
    goto END;
  } else if (_sendState == 11) {
    // ACK確認(確認しない) 終了
    goto DONE;
  } else {
    goto ERROR;
  }

ERROR:
  //Serial.println("Error");
  _err = _sendState;
DONE:
  _sendParity = 1;
  _sendState = 0;
  _flg_rs = 0;
END:
  return;
}

//
// CLKピン状態変化 ハンドラ
//
static volatile uint8_t state = 0;
void ICACHE_RAM_ATTR TPS2::clkPinHandle() {
  volatile static uint8_t data = 0;   // 受信データ
  volatile static uint8_t parity = 1; // パリティビット

  if (_flg_rs) {
    clkPinHandleSend(); // データ送信モード時は、サブルーチンで処理
    return;
  }

  if (Clk_In()) {
    goto END;
  }

  state++;
  if (state == 1) {	
    // [0] Stop bit (always LOW)
    if (Dat_In())
      goto ERROR;
  } else if (state >= 2 && state <= 9) {
    // [1-8] Data bits (for 8 bits)
    data >>= 1;
    if (Dat_In()) {
      data |= 0x80;
      parity++;
    }	   
  } else if (state == 10) {
    // Parity bit
    if (Dat_In()) {
      if (!(parity & 0x01))
        goto ERROR;
    } else {
      if (parity & 0x01)
        goto ERROR;
    }
  } else if (state == 11) {
    // Stop bit
    if (!Dat_In())
      goto ERROR;
    enqueue(data);
    goto DONE;
  } else {
    goto ERROR;	   
  }
  return;

ERROR:
  _err = state;
DONE:
  state = 0;
  data = 0;
  parity = 1;
END:
  return;
}

// キューのクリア
void TPS2::clear_queue() {
  _q_top = 0;
  _q_btm = 0;
}

void TPS2::resetState() {
  state = 0;
}

// キューへの挿入
void ICACHE_RAM_ATTR TPS2::enqueue(uint8_t data) {
  uint16_t n = (_q_top + 1) % QUEUESIZE;
  if (_q_top + 1 != _q_btm) {
    _queue[_q_top] = data;
    _q_top = n;
  }
}

// キューからの取出し
uint8_t TPS2::dequeue() {
  uint16_t val = 0;

  if (_q_top != _q_btm) {
    val = _queue[_q_btm];
    _q_btm = (_q_btm + 1) % QUEUESIZE;
  }
  return val;	
}

// 取出し可能チェック
uint8_t ICACHE_RAM_ATTR TPS2::available() {
  uint8_t d = _q_top != _q_btm ? 1 : 0;
  return d;
}
