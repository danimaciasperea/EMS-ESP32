/*
 * EMS-ESP - https://github.com/proddy/EMS-ESP
 * Copyright 2019  Paul Derbyshire
 * 
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/*
 * ESP32 UART port by @ArwedL and improved by @MichaelDvP. See https://github.com/proddy/EMS-ESP/issues/380
 */

#if defined(ESP32)

#include "uart/emsuart_esp32.h"

#include "emsesp.h"

namespace emsesp {

static intr_handle_t   uart_handle;
static RingbufHandle_t buf_handle   = NULL;
static hw_timer_t *    timer        = NULL;
bool                   drop_next_rx = true;
uint8_t                tx_mode_     = 0xFF;
uint8_t                emsTxBuf[EMS_MAXBUFFERSIZE];
uint8_t                emsTxBufIdx;
uint8_t                emsTxBufLen;
uint32_t               emsTxWait;

/*
* Task to handle the incoming data
*/
void EMSuart::emsuart_recvTask(void * para) {
    while (1) {
        size_t    item_size;
        uint8_t * telegram     = (uint8_t *)xRingbufferReceive(buf_handle, &item_size, portMAX_DELAY);
        uint8_t   telegramSize = item_size;

        if (telegram) {
            EMSESP::incoming_telegram(telegram, telegramSize);
            vRingbufferReturnItem(buf_handle, (void *)telegram);
        }
    }
}
/*
 * UART interrupt, on break read the fifo and put the whole telegram to ringbuffer
 */
void IRAM_ATTR EMSuart::emsuart_rx_intr_handler(void * para) {
    static uint8_t rxbuf[EMS_MAXBUFFERSIZE];
    static uint8_t length;

    if (EMS_UART.int_st.brk_det) {
        EMS_UART.int_clr.brk_det = 1; // clear flag
        length                   = 0;
        while (EMS_UART.status.rxfifo_cnt) {
            uint8_t rx = EMS_UART.fifo.rw_byte; // read all bytes from fifo
            if (length < EMS_MAXBUFFERSIZE) {
                rxbuf[length++] = rx;
            } else {
                drop_next_rx = true; // we have a overflow
            }
        }
        if ((!drop_next_rx) && ((length == 2) || (length > 4))) {
            int baseType = 0;
            xRingbufferSendFromISR(buf_handle, rxbuf, length - 1, &baseType);
        }
        drop_next_rx = false;
    }
}


void IRAM_ATTR EMSuart::emsuart_tx_timer_intr_handler() {
    if (emsTxBufIdx > 32) {
        return;
    }
    emsTxBufIdx++;
    if (emsTxBufIdx < emsTxBufLen) {
        EMS_UART.fifo.rw_byte = emsTxBuf[emsTxBufIdx];
        timerAlarmWrite(timer, emsTxWait, false);
        timerAlarmEnable(timer);
    } else  if (emsTxBufIdx == emsTxBufLen) {
        EMS_UART.conf0.txd_brk = 1; // <brk> after send
    }
}

/*
 * init UART driver
 */
void EMSuart::start(uint8_t tx_mode) {
    if(tx_mode > 10 ) {
        emsTxWait = EMSUART_BIT_TIME * tx_mode;
    } else if(tx_mode > 5 ) {
        emsTxWait = EMSUART_BIT_TIME * tx_mode * 2;
    } else if(tx_mode == 3) {
        emsTxWait = EMSUART_BIT_TIME * 17;
    } else if(tx_mode == 2) {
        emsTxWait = EMSUART_BIT_TIME * 20;
    } else if(tx_mode == 1) {
        emsTxWait = EMSUART_BIT_TIME * 11;
    }
    if (tx_mode_ != 0xFF) { // uart already initialized
        tx_mode_ = tx_mode;
        restart();
        return;
    }
    tx_mode_                  = tx_mode;
    uart_config_t uart_config = {
        .baud_rate = EMSUART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
    };
    uart_param_config(EMSUART_UART, &uart_config);
    if (tx_mode_ == 5) {
        EMS_UART.conf0.stop_bit_num = UART_STOP_BITS_1_5;
    }
    uart_set_pin(EMSUART_UART, EMSUART_TXPIN, EMSUART_RXPIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    EMS_UART.int_ena.val             = 0;          // disable all intr.
    EMS_UART.int_clr.val             = 0xFFFFFFFF; // clear all intr. flags
    EMS_UART.idle_conf.tx_brk_num    = 11;         // breaklength 11 bit
    EMS_UART.idle_conf.rx_idle_thrhd = 256;
    drop_next_rx                     = true;
    buf_handle                       = xRingbufferCreate(128, RINGBUF_TYPE_NOSPLIT);
    uart_isr_register(EMSUART_UART, emsuart_rx_intr_handler, NULL, ESP_INTR_FLAG_IRAM, &uart_handle);
    xTaskCreate(emsuart_recvTask, "emsuart_recvTask", 2048, NULL, configMAX_PRIORITIES - 1, NULL);
    EMS_UART.int_ena.brk_det = 1; // activate only break

    emsTxBufIdx = 0;
    emsTxBufLen = 0;
    timer = timerBegin(1, 80, true); // timer prescale to 1 µs, countup
	timerAttachInterrupt(timer, &emsuart_tx_timer_intr_handler, true); // Timer with edge interrupt
}

/*
 * Stop, disables interrupt
 */
void EMSuart::stop() {
    EMS_UART.int_ena.val = 0; // disable all intr.
	// timerAlarmDisable(timer);
};

/*
 * Restart Interrupt
 */
void EMSuart::restart() {
    if (EMS_UART.int_raw.brk_det) {
        EMS_UART.int_clr.brk_det = 1;    // clear flag
        drop_next_rx             = true; // and drop first frame
    }
    EMS_UART.int_ena.brk_det = 1; // activate only break
    emsTxBufIdx = 0;
    emsTxBufLen = 0;
    if (tx_mode_ == 5) {
        EMS_UART.conf0.stop_bit_num = UART_STOP_BITS_1_5;
    } else {
        EMS_UART.conf0.stop_bit_num = UART_STOP_BITS_1;
    }
}

/*
 * Sends a 1-byte poll, ending with a <BRK>
 */
void EMSuart::send_poll(uint8_t data) {
    if (tx_mode_ > 5 || tx_mode_ < 4) { // modes 1, 2, 3 also here
    // if (tx_mode_ > 5) {
        EMS_UART.fifo.rw_byte = data;
        emsTxBufIdx = 0;
        emsTxBufLen = 1;
        timerAlarmWrite(timer, emsTxWait, false);
        timerAlarmEnable(timer);
    } else if (tx_mode_ >= EMS_TXMODE_NEW) {
        EMS_UART.fifo.rw_byte  = data;
        EMS_UART.conf0.txd_brk = 1; // <brk> after send
    } else if (tx_mode_ == EMS_TXMODE_HT3) {
        EMS_UART.fifo.rw_byte  = data;
        delayMicroseconds(EMSUART_TX_WAIT_HT3);
        EMS_UART.conf0.txd_brk = 1; // <brk>
        // delayMicroseconds(EMSUART_TX_WAIT_BRK);
        // EMS_UART.conf0.txd_brk = 0;
    } else if (tx_mode_ == EMS_TXMODE_EMSPLUS) {
        EMS_UART.fifo.rw_byte  = data;
        delayMicroseconds(EMSUART_TX_WAIT_PLUS);
        EMS_UART.conf0.txd_brk = 1; // <brk>
        // delayMicroseconds(EMSUART_TX_WAIT_BRK);
        // EMS_UART.conf0.txd_brk = 0;
    } else {
        volatile uint8_t _usrxc = EMS_UART.status.rxfifo_cnt;
        EMS_UART.fifo.rw_byte   = data;
        while (EMS_UART.status.rxfifo_cnt == _usrxc) {
        }
        EMS_UART.conf0.txd_brk = 1; // <brk>
    }
}

/*
 * Send data to Tx line, ending with a <BRK>
 * buf contains the CRC and len is #bytes including the CRC
 * returns code, 1=success
 */
uint16_t EMSuart::transmit(uint8_t * buf, uint8_t len) {
    if (len == 0 || len >= EMS_MAXBUFFERSIZE) {
        return EMS_TX_STATUS_ERR;
    }
    if (tx_mode_ > 5 || tx_mode_ < 4) { // timer controlled modes, also modes 1, 2, 3 because delays not working
    // if (tx_mode_ > 5) { // timer controlled modes
        for (uint8_t i = 0; i < len; i++) {
            emsTxBuf[i] = buf[i];
        }
        EMS_UART.fifo.rw_byte = buf[0];
        emsTxBufIdx = 0;
        emsTxBufLen = len;
        timerAlarmWrite(timer, emsTxWait, false);
        timerAlarmEnable(timer);
        return EMS_TX_STATUS_OK;
    }
    if (tx_mode_ >= EMS_TXMODE_NEW) { // hardware controlled modes
        for (uint8_t i = 0; i < len; i++) {
            EMS_UART.fifo.rw_byte = buf[i];
        }
        EMS_UART.conf0.txd_brk = 1; // <brk> after send
        return EMS_TX_STATUS_OK;
    }
    if (tx_mode_ == EMS_TXMODE_EMSPLUS) { // EMS+ with long delay
        for (uint8_t i = 0; i < len; i++) {
            EMS_UART.fifo.rw_byte = buf[i];
            delayMicroseconds(EMSUART_TX_WAIT_PLUS);
        }
        EMS_UART.conf0.txd_brk = 1; // <brk> after send, cleard by hardware after send
        // delayMicroseconds(EMSUART_TX_WAIT_BRK);
        // EMS_UART.conf0.txd_brk = 0;
        return EMS_TX_STATUS_OK;
    }
    if (tx_mode_ == EMS_TXMODE_HT3) { // HT3 with 7 bittimes delay
        for (uint8_t i = 0; i < len; i++) {
            EMS_UART.fifo.rw_byte = buf[i];
            delayMicroseconds(EMSUART_TX_WAIT_HT3);
        }
        EMS_UART.conf0.txd_brk = 1; // <brk> after send, cleard by hardware after send
        // delayMicroseconds(EMSUART_TX_WAIT_BRK);
        // EMS_UART.conf0.txd_brk = 0;
        return EMS_TX_STATUS_OK;
    }
    // mode 1 
    // flush fifos -- not supported in ESP32 uart #2!
    // EMS_UART.conf0.rxfifo_rst = 1;
    // EMS_UART.conf0.txfifo_rst = 1;
    for (uint8_t i = 0; i < len; i++) {
        volatile uint8_t _usrxc = EMS_UART.status.rxfifo_cnt;
        EMS_UART.fifo.rw_byte    = buf[i]; // send each Tx byte
        // wait for echo
        while (EMS_UART.status.rxfifo_cnt == _usrxc) {
            // delayMicroseconds(EMSUART_TX_BUSY_WAIT); // burn CPU cycles...
        }
    }
    EMS_UART.conf0.txd_brk = 1; // <brk> after send, cleard by hardware after send
    // delayMicroseconds(EMSUART_TX_WAIT_BRK);
    // EMS_UART.conf0.txd_brk = 0;
    return EMS_TX_STATUS_OK;
}

} // namespace emsesp

#endif
