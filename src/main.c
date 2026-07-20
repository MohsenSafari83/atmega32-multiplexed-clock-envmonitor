#include <mega32.h>
#include <stdint.h>
// Declare your global variables here
// 7-segment common-anode codes: 0-9, blank, minus sign, unit letters
#define BLANK   10
#define MINUS   11
#define CHAR_C  12
#define CHAR_F  13
#define CHAR_H  14

#define NUM_DIGITS 6

// display / operating modes
enum { MODE_TIME, MODE_TEMP, MODE_FAHR, MODE_HUM };

unsigned char seg_code[] = {
    0xC0, 0xF9, 0xA4, 0xB0, 0x99, 0x92, 0x82, 0xF8, 0x80, 0x90, // 0-9
    0xFF,   // 10 blank
    0xBF,   // 11 minus
    0xC6,   // 12 'C'
    0x8E,   // 13 'F'
    0x8A    // 14 'H'
};
// --- display buffer / multiplexing ---
volatile unsigned char display_buf[NUM_DIGITS];
volatile unsigned char current_digit = 0;
volatile uint8_t blink[NUM_DIGITS];   // blink[i] = 1 -> digit i should blink
volatile uint16_t tick = 0;            // bumped once per refresh ISR
unsigned char show;

// --- mode switching ---
volatile uint8_t mode = MODE_TIME;

// --- Clock (time-of-day) state ---
volatile uint8_t hours   = 12;
volatile uint8_t minutes = 34;
volatile uint8_t seconds = 0;

// --- Clock edit state ---
volatile uint8_t clock_edit = 0;   // 0 = run, 1 = edit
volatile uint8_t cursor     = 0;   // 0 = hours, 1 = minutes, 2 = seconds

// --- UART / temperature-sensor state ---
volatile signed char   current_temp  = 0;   // updated whenever a reply arrives
volatile unsigned char temp_request  = 0;   // set once per minute (or on mode change)
// -----KEYPAD / CLOCK-EDIT FUNCTIONS-----

void apply_keys(uint8_t pressed)
{
    if (!clock_edit) return;

    if (pressed & 0x04) { if (cursor < 2) cursor++; }     // RIGHT
    if (pressed & 0x08) { if (cursor > 0) cursor--; }     // LEFT

    if (pressed & 0x01)   // UP
    {
        if (cursor == 0)      { if (++hours   >= 24) hours   = 0; }
        else if (cursor == 1) { if (++minutes >= 60) minutes = 0; }
        else                  { if (++seconds >= 60) seconds = 0; }
    }
    if (pressed & 0x02)   // DOWN
    {
        if (cursor == 0)      { if (hours   == 0) hours   = 23; else hours--; }
        else if (cursor == 1) { if (minutes == 0) minutes = 59; else minutes--; }
        else                  { if (seconds == 0) seconds = 59; else seconds--; }
    }
}

void scan_edit_keys(void)
{
    static unsigned char prev = 0x0F;
    unsigned char curr = PINA & 0x0F;              // active low (0 = pressed)
    unsigned char falling = (prev ^ curr) & prev;  // was 1, now 0 = just pressed
    prev = curr;

    if (falling)
        apply_keys(falling);
}

// --------DISPLAY FUNCTIONS------

void set_blink(unsigned char i, unsigned char on)
{
    blink[i] = on;
}

void show_time(void)
{
    display_buf[0] = hours / 10;
    display_buf[1] = hours % 10;
    display_buf[2] = minutes / 10;
    display_buf[3] = minutes % 10;
    display_buf[4] = seconds / 10;
    display_buf[5] = seconds % 10;
}

// Formats a signed value (temperature or humidity) with its unit letter
void draw_env_value(signed char val, unsigned char unit)
{
    if (val < 0)
    {
        display_buf[0] = MINUS;
        val = -val;
    }
    else
        display_buf[0] = (val >= 100) ? (val / 100) : BLANK;

    if (val >= 100)
    {
        display_buf[1] = (val / 10) % 10;
        display_buf[2] = val % 10;
    }
    else if (val >= 10)
    {
        display_buf[1] = val / 10;
        display_buf[2] = val % 10;
    }
    else
    {
        display_buf[1] = BLANK;
        display_buf[2] = val;
    }

    display_buf[3] = BLANK;
    display_buf[4] = BLANK;
    display_buf[5] = unit;
}

void show_temperature(void) { draw_env_value(current_temp, CHAR_C); }
void show_fahr(void)        { draw_env_value(current_temp, CHAR_F); }
void show_humidity(void)    { draw_env_value(current_temp, CHAR_H); }

// Fills display_buf according to the current mode, and sets up blink flags
void update_display(void)
{
    unsigned char i;
    for (i = 0; i < NUM_DIGITS; i++) set_blink(i, 0);   // always start clean

    switch (mode)
    {
        case MODE_TIME:
            if (clock_edit)
            {
                if (cursor == 0)      { set_blink(0, 1); set_blink(1, 1); }
                else if (cursor == 1) { set_blink(2, 1); set_blink(3, 1); }
                else                  { set_blink(4, 1); set_blink(5, 1); }
            }
            show_time();
            break;

        case MODE_TEMP: show_temperature(); break;
        case MODE_FAHR: show_fahr();        break;
        case MODE_HUM:  show_humidity();    break;
    }
}
// -----INTERRUPT SERVICE ROUTINES-----
// External Interrupt 0 — cycles the display mode and requests a fresh reading
interrupt [EXT_INT0] void ext_int0_isr(void)
{
    if (++mode > MODE_HUM) mode = MODE_TIME;
    temp_request = 1;
}

// External Interrupt 1 — toggles clock edit mode
interrupt [EXT_INT1] void ext_int1_isr(void)
{
    clock_edit ^= 1;
}

// Timer1 Compare Match A — 1-second tick, advances the running clock
interrupt [TIM1_COMPA] void timer1_compa_isr(void)
{
    if (++seconds >= 60)
    {
        seconds = 0;
        temp_request = 1; // request a fresh sensor reading every minute
        if (++minutes >= 60)
        {
            minutes = 0;
            if (++hours >= 24) hours = 0;
        }
    }
}

// USART RX Complete — stores whatever byte the sensor board sends back
interrupt [USART_RXC] void usart_rx_isr(void)
{
    current_temp = (signed char)UDR;
}

// Timer2 Compare Match — display refresh / digit multiplexing + key scan
interrupt [TIM2_COMP] void timer2_comp_isr(void)
{
    static unsigned int heartbeat_tick = 0;

    scan_edit_keys();

    PORTC = 0x00;

    show = 1;
    if (blink[current_digit] && ((tick >> 6) & 1))
        show = 0;
    tick++;

    if (show)
        PORTB = seg_code[display_buf[current_digit]];
    else
        PORTB = seg_code[BLANK];

    if (mode == MODE_TIME)
    {
        if ((current_digit == 2 || current_digit == 4) && (seconds & 1))
            PORTB &= 0x7F;
    }

    PORTC = (1 << current_digit);

    if (++current_digit >= NUM_DIGITS)
        current_digit = 0;

    if (++heartbeat_tick >= 500)
    {
        heartbeat_tick = 0;
        PORTD ^= 0x10;
    }
}


//------ UART FUNCTIONS---------

// USART initialization: 9600 baud @ 8 MHz, 8-N-1, RX+TX+RXCIE enabled
void uart_init(void)
{
    UBRRH = 0x00;
    UBRRL = 0x33;

    UCSRB = (1 << RXCIE) | (1 << RXEN) | (1 << TXEN);
    UCSRC = (1 << URSEL) | (1 << UCSZ1) | (1 << UCSZ0);
}

// Blocking single-byte UART send
void uart_send(uint8_t b)
{
    while (!(UCSRA & (1 << UDRE)));   // wait for TX buffer empty
    UDR = b;
}

// Sends the correct request byte ('C'/'F'/'H') once per minute or on mode change
void poll_temperature(void)
{
    if (!temp_request)
        return;

    temp_request = 0;

    if (mode == MODE_TEMP)      uart_send('C');
    else if (mode == MODE_FAHR) uart_send('F');
    else if (mode == MODE_HUM)  uart_send('H');
}

void main(void)
{
// Declare your local variables here

// Input/Output Ports initialization
// Port A initialization
// Function: Bit7=Out Bit6=Out Bit5=Out Bit4=Out Bit3=Out Bit2=Out Bit1=Out Bit0=Out 
DDRA=0x00; 
// State: Bit7=0 Bit6=0 Bit5=0 Bit4=0 Bit3=0 Bit2=0 Bit1=0 Bit0=0 
PORTA=(1<<PORTA3)|(1<<PORTA2)|(1<<PORTA1)|(1<<PORTA0);  

// Port B initialization
// Function: Bit7=In Bit6=In Bit5=In Bit4=In Bit3=In Bit2=In Bit1=In Bit0=Out 
DDRB=0xFF;  
// State: Bit7=T Bit6=T Bit5=T Bit4=T Bit3=T Bit2=T Bit1=T Bit0=0 
PORTB=0x00; 

// Port C initialization
// Function: Bit7=In Bit6=In Bit5=In Bit4=In Bit3=Out Bit2=Out Bit1=Out Bit0=Out 
DDRC = 0x3F; 
// State: Bit7=T Bit6=T Bit5=T Bit4=T Bit3=0 Bit2=0 Bit1=0 Bit0=0 
PORTC=0x00;
// Port D initialization
// Function: Bit7=In Bit6=In Bit5=In Bit4=In Bit3=In Bit2=In Bit1=In Bit0=Out 
DDRD = (1<<DDD4) | (1<<DDD1);
// State: Bit7=T Bit6=T Bit5=T Bit4=T Bit3=T Bit2=T Bit1=T Bit0=0 
PORTD = 0x0C; 
// Timer/Counter 0 initialization
// Clock source: System Clock
// Clock value: Timer 0 Stopped
// Mode: Normal top=0xFF
// OC0 output: Disconnected
TCCR0=(0<<WGM00) | (0<<COM01) | (0<<COM00) | (0<<WGM01) | (0<<CS02) | (0<<CS01) | (0<<CS00);
TCNT0=0x00;
OCR0=0x00;

// Timer/Counter 1 initialization
// Clock source: System Clock
// Clock value: Timer1 Stopped
// Mode: Normal top=0xFFFF
// OC1A output: Disconnected
// OC1B output: Disconnected
// Noise Canceler: Off
// Input Capture on Falling Edge
// Timer1 Overflow Interrupt: Off
// Input Capture Interrupt: Off
// Compare A Match Interrupt: Off
// Compare B Match Interrupt: Off
TCCR1A=(0<<COM1A1) | (0<<COM1A0) | (0<<COM1B1) | (0<<COM1B0) | (0<<WGM11) | (0<<WGM10);
TCCR1B = (1 << WGM12) | (1 << CS12); 
TCNT1H=0x00;
TCNT1L=0x00;
ICR1H=0x00;
ICR1L=0x00;
OCR1AH = 0x7A;
OCR1AL = 0x11;
OCR1BH=0x00;
OCR1BL=0x00;

// Timer/Counter 2 initialization
// Clock source: System Clock
// Clock value: 125.000 kHz
// Mode: CTC top=OCR2A
// OC2 output: Disconnected
// Timer Period: 2 ms
ASSR=0<<AS2;
TCCR2=(0<<PWM2) | (0<<COM21) | (0<<COM20) | (1<<CTC2) | (1<<CS22) | (0<<CS21) | (0<<CS20);
TCNT2=0x00;
OCR2=0xF9;

// Timer(s)/Counter(s) Interrupt(s) initialization
TIMSK  = (1 << OCIE1A) | (1 << OCIE2);
// External Interrupt(s) initialization
// INT0: On
// INT0 Mode: Falling Edge
// INT1: On
// INT1 Mode: Falling Edge
// INT2: Off
GICR|=(1<<INT1) | (1<<INT0) | (0<<INT2);
MCUCR=(1<<ISC11) | (0<<ISC10) | (1<<ISC01) | (0<<ISC00);
MCUCSR=(0<<ISC2);
GIFR=(1<<INTF1) | (1<<INTF0) | (0<<INTF2);

// USART initialization
// Communication Parameters: 8 Data, 1 Stop, No Parity
// USART Receiver: On
// USART Transmitter: On
// USART Mode: Asynchronous
// USART Baud Rate: 9600
// Analog Comparator initialization
// Analog Comparator: Off
// The Analog Comparator's positive input is
// connected to the AIN0 pin
// The Analog Comparator's negative input is
// connected to the AIN1 pin
ACSR=(1<<ACD) | (0<<ACBG) | (0<<ACO) | (0<<ACI) | (0<<ACIE) | (0<<ACIC) | (0<<ACIS1) | (0<<ACIS0);
SFIOR=(0<<ACME);

// ADC initialization
// ADC disabled
ADCSRA=(0<<ADEN) | (0<<ADSC) | (0<<ADATE) | (0<<ADIF) | (0<<ADIE) | (0<<ADPS2) | (0<<ADPS1) | (0<<ADPS0);

// SPI initialization
// SPI disabled
SPCR=(0<<SPIE) | (0<<SPE) | (0<<DORD) | (0<<MSTR) | (0<<CPOL) | (0<<CPHA) | (0<<SPR1) | (0<<SPR0);

// TWI initialization
// TWI disabled
TWCR=(0<<TWEA) | (0<<TWSTA) | (0<<TWSTO) | (0<<TWEN) | (0<<TWIE);
// USART initialization
uart_init();
// Global enable interrupts
#asm("sei")
update_display();
    while (1)
    {
        poll_temperature();
        update_display();
    }
}