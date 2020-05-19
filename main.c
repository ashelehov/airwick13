//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//																				
//  						'Tiny 13A led sensor AirWick' by Alex Shelehov (C)
//												v.1.2 light
//
//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

// Частота контроллера
#define F_CPU 1200000UL

// Библиотеки
#include <avr/io.h>
#include <avr/interrupt.h>
#include <avr/wdt.h>
#include <avr/sleep.h>
#include <avr/eeprom.h>
#include <util/delay.h>
#include <stdbool.h>

// Пины
#define PIN_MOTOR 			PB0	
#define PIN_BUTTON_MOTOR 	PB1
#define PIN_BUTTON_MODE 	PB2
#define PIN_LED_N 			PB3
#define PIN_LED_P 			PB4

// множитель wdt n=9.6 (время в секундах делим на n)

#define MOTOR_WORK_TIME 	500						// продолжительность работы мотора, миллисекунд, БЕЗ МНОЖИТЕЛЯ!
#define MIN_LIGHT_TIME		16						// 2.5 минуты = 16, 5 минут = 31. сколько должен гореть свет, чтобы считать, что нужно брызгать
#define MAX_LIGHT_TIME		375						// сколько секунд должен гореть свет, чтобы считать, что его забыли выключить (1 час = 375)
#define MIN_LAST_TIME		94						// Минимальное время после последнего "пшика", после которого может быть новый, 15 мин = 94
#define DEFAULT_MODE		1						// режим таймера по молчанию, если в EEPROM пусто (индекс от нуля)
#define MODE_COUNT			3						// Число режимов таймера


uint8_t Addr EEMEM;									// регистрируем переменную в EEPROM по адресу "Addr"	

const uint16_t motor_on_time[MODE_COUNT] = 
	{ 375, 1125, 2250 }; 							// 1 час = 375, 3 часа, 6 часов. Через какое время срабатывает мотор (автоматически, по таймеру)

volatile bool sleep_flag = false;
volatile bool button_motor_flag = false;
volatile bool button_mode_flag = false;

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//										ПРЕРЫВАНИЯ И ФУНКЦИИ
//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

// разрешаем прерывания для кнопок
void button_interrupts_enable(void)
{
	GIMSK = _BV(PCIE); 							
	PCMSK = _BV(PIN_BUTTON_MOTOR) | _BV(PIN_BUTTON_MODE); 			// Аппаратные пины для прерываний кнопок
	//PCMSK = _BV(PIN_BUTTON_MODE);
}

// настраиваем 'watch dog timer'
void wdt_setup(void)
{
	wdt_reset();
	WDTCR |= _BV(WDCE) | _BV(WDE);   	// разрешаем настройку ватчдога
	WDTCR = _BV(WDTIE) |             	// разрешаем прерывание WDT, _BV(WDIE) для attiny85
		_BV(WDP3) | _BV(WDP0); 			// выбираем время таймера 8s _BV(WDP3) | _BV(WDP0);  1s _BV(WDP2) | _BV(WDP1);
}

// отправляем контроллер в сон
void mk_sleep_enable(void)
{
	ACSR |= _BV(ACD);                   			
	ADCSRA &= ~_BV(ADEN);	
	
	set_sleep_mode(SLEEP_MODE_PWR_DOWN);    		
	sleep_enable();
}

// Срабатывает WDT
ISR(WDT_vect)
{
	sleep_flag = false;
}

// Нажали кнопку
ISR(PCINT0_vect)
{ 
	if (PINB & _BV(PIN_BUTTON_MODE))  button_mode_flag  = true;
	if (PINB & _BV(PIN_BUTTON_MOTOR)) button_motor_flag = true;
}

// Замеряем яркость
uint16_t readLED(void)
{
	uint16_t j;
	PORTB |= _BV(PIN_LED_N);						// Даем обратное напряжение на диод
	_delay_us(1000);								// даем время зарядиться паразитной емкости
	DDRB  &= ~_BV(PIN_LED_N); 						// Устанавливаем пин как вход
	PORTB &= ~_BV(PIN_LED_N); 						// снимаем напряжение с диода
	for (j=0; j<30000; j++)
	{
		if (!(PINB & _BV(PIN_LED_N))) break; 		// считаем время, пока заряд уйдет, чем ярче, тем быстрей (0-65000)
	}
	DDRB |= _BV(PIN_LED_N); 						// пин как выход
	return j;
}

// мигаем диодом
void led_blink(void)
{
	PORTB |= _BV(PIN_LED_P); 			
	_delay_us(1000);
	PORTB &= ~_BV(PIN_LED_P);
}

// Включение/выключение мотора
void motor_work(void)
{
	for (uint8_t t=0; t<10; t++)
	{ 
		led_blink();								// Мигаем перед включением мотора
		_delay_ms(200); 
	}
	
	cli();											// запрещаем прерывания пока включен мотор
	
	PORTB |= _BV(PIN_MOTOR); 
	_delay_ms(MOTOR_WORK_TIME);
	PORTB &= ~_BV(PIN_MOTOR);
	
	sei();
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//										ОСНОВНАЯ ПРОГРАММА
//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

int main(void)
{
	uint16_t main_timer = 0; 						// главный (общий) таймер
	uint16_t light_timer = 0; 						// таймер включенного света
	uint8_t mode;									// текущий режим	
	uint16_t light_level, light_limit;				// уровень освещенности на датчике. Порог освещенности. 0-самый яркий свет, 65000-темнота
	bool light_on_flag = false;	
	
	
	/*----- SETUP -----*/
	
	DDRB  = 0xFF; 									// Все пины как "выход"					
	PORTB = 0x00;									// Притягиваем все пины к "земле"

	// Калибруем датчик при включении. задержка 10 сек. Предварительно мигаем диодом
	for (uint8_t t=0; t<10; t++)
	{ 
		led_blink();
		_delay_ms(1000); 
	}
	
	light_limit = readLED();						// Определяем текущую яркость. 
	light_limit += 5000; 							// Настраиваем порог на её основе.
	
	mode = eeprom_read_byte(&Addr);					// Читаем значение режима из EEPROM
	if (mode == 0xFF) mode = DEFAULT_MODE;			// Если значение не установлено (0xff), используем режим по-умолчанию	
	
	cli();
	
	button_interrupts_enable();
	mk_sleep_enable();
	wdt_setup();
	
	sei();
	
	//motor_work();									// делаем тестовый пшик
	
	/*----- LOOP -----*/
	
	while(1)
	{
		// Нажата кнопка MOTOR
		if (button_motor_flag)
		{
			motor_work();
			main_timer = 0;
			button_motor_flag = false;
		}	
		
		// Нажата кнопка MODE
		if (button_mode_flag)
		{
			cli();									// отключаем и потом заново включаем прерывания при записи в EEPROM
			
			mode++;
			if (mode == MODE_COUNT) mode = 0;		// меняем режимы по кругу
			
			eeprom_write_byte(&Addr, mode);			// пишем в EEPROM текущий режим
			
			for (uint8_t t=0; t<mode+1; t++)
			{
				led_blink();						// Мигаем диодом в соответсвии с выбранным режимом
				_delay_ms(500);
			}
			
			main_timer = 0;							// сбрасываем таймеры
			light_timer = 0;
			_delay_ms(2000);
			
			sei();
			
			button_mode_flag = false;
		}	
		
		// Если сработал таймер Watch Dog (WDT)
		if (!sleep_flag)
		{			
			main_timer++;
			
			light_level = readLED();				// измеряем яркость	
			
			led_blink();							// мигаем диодом
			
			// если прошло время общего таймера
			if (main_timer >= motor_on_time[mode])
			{
				// Если свет выкл. или свет вкл., но прошло время: пшикаем (таймер сбросится), иначе просто сбрасываем общий таймер 
				if (light_level >= light_limit || light_timer >= MAX_LIGHT_TIME) 
				{
					motor_work();
				} 

				main_timer = 0;
			}
			
			// Делаем ПШИК, если включали и выключили свет

			// Если свет включен
			if (light_level < light_limit)
			{
				if (!light_on_flag)
				{ 
					_delay_ms(200); 
					led_blink(); 					// При включении света мигаем второй раз
				}

				light_timer++;
				if (light_timer > MAX_LIGHT_TIME) light_timer = MAX_LIGHT_TIME; 	// защищаем таймер от переполнения
				light_on_flag = true;
			} 
			else 
			{
				// Если выключен но до этого был включен, 
				if (light_on_flag)
				{
					// и прошло время задержки и с моменты последнего пшика прошло нужное время, чтобы не пшикал слишком часто
					if (light_timer > MIN_LIGHT_TIME && main_timer > MIN_LAST_TIME)
					{ 
						motor_work();
						main_timer = 0;
					} 
					else 
					{ 
						_delay_ms(200); 
						led_blink(); 				// при выключении света мигаем второй раз
					} 	
					
					light_timer = 0;				// сбрасываем таймер света
					light_on_flag = false;
				}
			}
			
			sleep_flag = true;	
		} 
		
		sleep_cpu(); 								// ложимся спать
	} 
	
	return 0;
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
//                              А Л Г О Р И Т М    Р А Б О Т Ы : 											
//																									
//	1) пшикаем через равные промежутки времени, напр. раз в час, при условии, что свет выключен,			
//		то есть в помещении никого нет.																		
//	2) если свет включается, запускаем таймер.																
//	3) когда свет выключается:																				
//		1. если прошло мало времени (напр. меньше 3 минут), значит дела не сделали и пшикать не надо.		
//		2. если прошло больше, пшикаем и сбрасываем таймер.
//			если после последнего пшика прошло меньше 15 минут, то не брызгаем, так как аэрозоль еще 
//			не выветрился.												
//	4) если свет долго не выключается (напр. больше часа), значит забыли выключить, продолжаем брызгать.	
// 
//	Тестовый "пшик" по-умолчанию отключен.		
//																								
//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
// 								О П И С А Н И Е   Р Е Ж И М О В :
//
// 1) Пшикаем 1 раз в час, минимальное время включения света - 2.5 минуты
// 2) -//- каждые 3 часа, 
// 3) -//- каждые 6 часов. 
//
//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
//								(C) Алексей Шелехов '2020
//
//						 Только для некомерческого использования
//
//						ashelehov@yandex.ru  http://wedfotoart.ru
//
//	Программа предоставляется "как есть", используйте на свой страх и риск, любые претензии не принимаются!!!
//
//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
