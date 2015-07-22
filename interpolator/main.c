#include "stm32f4xx_conf.h"
#include "stm32f4_discovery.h"
#include "math.h"
#include "arm_math.h"
#include "cnc.h"

static const struct {
    GPIO_TypeDef *gpio;
    uint16_t xDirection, xStep, yDirection, yStep, zDirection, zStep;
} motorsPinout = {
        .gpio = GPIOE,
        .xStep = GPIO_Pin_3,
        .xDirection = GPIO_Pin_4,
        .yStep = GPIO_Pin_5,
        .yDirection = GPIO_Pin_6,
        .zStep = GPIO_Pin_7,
        .zDirection = GPIO_Pin_8};

static const struct {
    GPIO_TypeDef *gpio;
    uint16_t eStopButton, eStopLed;
    uint16_t stopInterruptLine;
    uint8_t stopIrqN;
    uint8_t extiPortSource, extiPinSource;
} eStopPinout = {
        .gpio = GPIOE,
        //pin, EXTI line and EXTI IRQn should be the same, it's just poor API design
        .eStopButton = GPIO_Pin_14,
        .eStopLed = GPIO_Pin_15,
        .stopInterruptLine = EXTI_Line14,
        .stopIrqN = EXTI15_10_IRQn,
        .extiPortSource = EXTI_PortSourceGPIOE,
        .extiPinSource = EXTI_PinSource14
};

volatile cnc_memory_t cncMemory = {
        .position = {.x = 0, .y = 0, .z = 0, .speed = 0},
        .parameters = {
                .stepsPerMillimeter = 640,
                .maxSpeed = 3000,
                .maxAcceleration = 100,
                .clockFrequency = 200000},
        .state = READY,
        .lastEvent = {NULL_EVENT, 0, 0, 0},
        .tick = 0,
        .stepperState = NOT_RUNNING};

static const struct {
    unsigned int x:1, y:1, z:1;
} motorDirection = {
        .x = 1,
        .y = 1,
        .z = 0};

static step_t nextProgramStep() {
    uint8_t bytes[3];
    if (!readFromProgram(sizeof(bytes) / sizeof(*bytes), bytes))
        return (step_t) {.duration = 0,
                .axes = {
                        .xStep = 0,
                        .yStep = 0,
                        .zStep = 0}};
    uint8_t binAxes = bytes[2];
    return (step_t) {
            .duration = bytes[1] << 8 | bytes[0],
            .axes = {
                    .xStep = (uint8_t) ((binAxes & 0b000001) != 0),
                    .yStep = (uint8_t) ((binAxes & 0b000100) != 0),
                    .zStep = (uint8_t) ((binAxes & 0b010000) != 0),
                    .xDirection = (uint8_t) ((binAxes & 0b000010) != 0),
                    .yDirection = (uint8_t) ((binAxes & 0b001000) != 0),
                    .zDirection = (uint8_t) ((binAxes & 0b100000) != 0)}};
}

static int xor(int a, int b) {
    return (a && !b) || (!a && b);
}

static void executeStep(step_t step) {
    //diagonal steps are longer than straight ones
    static float32_t stepFactors[] = {0, 1, 1.414213562f, 1.732050808f};
    float32_t minDuration = cncMemory.parameters.clockFrequency /
            (cncMemory.parameters.maxSpeed * cncMemory.parameters.stepsPerMillimeter / 60);
    GPIO_ResetBits(motorsPinout.gpio, motorsPinout.xDirection | motorsPinout.xStep
            | motorsPinout.yDirection | motorsPinout.yStep | motorsPinout.zDirection | motorsPinout.zStep);
    cncMemory.currentStep = step;
    if (step.duration) {
        STM_EVAL_LEDOn(LED6);
        uint16_t directions = 0;
        if (xor(cncMemory.currentStep.axes.xDirection, motorDirection.x))
            directions |= motorsPinout.xDirection;
        if (xor(cncMemory.currentStep.axes.yDirection, motorDirection.y))
            directions |= motorsPinout.yDirection;
        if (xor(cncMemory.currentStep.axes.zDirection, motorDirection.z))
            directions |= motorsPinout.zDirection;
        GPIO_SetBits(motorsPinout.gpio, directions);
        cncMemory.stepperState = DIRECTION_SET;
        uint32_t duration = step.duration;
        int32_t axesCount = step.axes.xStep + step.axes.yStep + step.axes.zStep;
        float32_t stepFactor = stepFactors[axesCount];
        uint32_t correctedMinDuration = (uint32_t) ceilf(minDuration * stepFactor);
        correctedMinDuration = correctedMinDuration < 2 ? 2 : correctedMinDuration;
        if (cncMemory.state != MANUAL_CONTROL)
            //clamp speed according to max allowed speed
            duration = duration < correctedMinDuration ? correctedMinDuration : duration;
        cncMemory.position.speed = (int32_t) (stepFactor == 0 ? 0 : duration / stepFactor);
        TIM3->ARR = duration;
        TIM3->CNT = 0;
        TIM_SelectOnePulseMode(TIM3, TIM_OPMode_Single);
        TIM_Cmd(TIM3, ENABLE);
    }
}

void executeNextStep() {
    if (cncMemory.state == MANUAL_CONTROL)
        executeStep(nextManualStep());
    else if (cncMemory.state == RUNNING_PROGRAM)
        executeStep(nextProgramStep());
    else
        cncMemory.position.speed = 0;
}

void updatePosition(step_t step) {
    if (step.axes.xStep)
        cncMemory.position.x += step.axes.xDirection ? 1 : -1;
    if (step.axes.yStep)
        cncMemory.position.y += step.axes.yDirection ? 1 : -1;
    if (step.axes.zStep)
        cncMemory.position.z += step.axes.zDirection ? 1 : -1;
}

int stepTimeHasCome() {
    return TIM_GetITStatus(TIM3, TIM_IT_CC1) != RESET;
}

void clearStepTimeHasCome() {
    TIM_ClearITPendingBit(TIM3, TIM_IT_CC1);
}

int stepIsOver() {
    return TIM_GetITStatus(TIM3, TIM_IT_Update) != RESET;
}

void clearStepIsOver() {
    TIM_ClearITPendingBit(TIM3, TIM_IT_Update);
}

uint32_t isEmergencyStopped() {
    return (uint32_t) !GPIO_ReadInputDataBit(eStopPinout.gpio, eStopPinout.eStopButton);
}

__attribute__ ((noreturn)) void main(void) {
    //enable FPU
    SCB->CPACR |= 0b000000000111100000000000000000000UL;

    STM_EVAL_LEDInit(LED3);
    STM_EVAL_LEDInit(LED4);
    STM_EVAL_LEDInit(LED5);
    STM_EVAL_LEDInit(LED6);
    STM_EVAL_LEDOff(LED3);
    STM_EVAL_LEDOff(LED4);
    STM_EVAL_LEDOff(LED5);
    STM_EVAL_LEDOff(LED6);

    RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOE, ENABLE);

    GPIO_Init(motorsPinout.gpio, &(GPIO_InitTypeDef) {
            .GPIO_Pin = motorsPinout.xDirection | motorsPinout.xStep
                    | motorsPinout.yDirection | motorsPinout.yStep
                    | motorsPinout.zDirection | motorsPinout.zStep,
            .GPIO_Mode = GPIO_Mode_OUT,
            .GPIO_Speed = GPIO_Speed_2MHz,
            .GPIO_OType = GPIO_OType_PP,
            .GPIO_PuPd = GPIO_PuPd_NOPULL});
    GPIO_Init(eStopPinout.gpio, &(GPIO_InitTypeDef) {
            .GPIO_Pin = eStopPinout.eStopButton,
            .GPIO_Mode = GPIO_Mode_IN,
            .GPIO_Speed = GPIO_Speed_2MHz,
            .GPIO_PuPd = GPIO_PuPd_DOWN
    });

    RCC_APB1PeriphClockCmd(RCC_APB1Periph_TIM3, ENABLE);
    TIM_Cmd(TIM3, DISABLE);
    TIM_UpdateRequestConfig(TIM3, TIM_UpdateSource_Regular);
    TIM_SelectOnePulseMode(TIM3, TIM_OPMode_Single);
    TIM3->CNT = 0;
    TIM_TimeBaseInit(TIM3, &((TIM_TimeBaseInitTypeDef) {
            .TIM_Period = 10000,
            .TIM_Prescaler = (uint16_t) ((SystemCoreClock / 2) / cncMemory.parameters.clockFrequency) - 1,
            .TIM_ClockDivision = 0,
            .TIM_CounterMode = TIM_CounterMode_Up}));
    /* Channel1 for step */
    TIM_OC1Init(TIM3, &(TIM_OCInitTypeDef) {
            .TIM_OCMode = TIM_OCMode_PWM1,
            .TIM_OutputState = TIM_OutputState_Enable,
            .TIM_Pulse = 1,
            .TIM_OCPolarity = TIM_OCPolarity_High});
    TIM_OC1PreloadConfig(TIM3, TIM_OCPreload_Disable);
    TIM_ITConfig(TIM3, TIM_IT_CC1 | TIM_IT_Update, ENABLE);

    initUSB();
    initManualControls();
    SysTick_Config(SystemCoreClock / cncMemory.parameters.clockFrequency);

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wmissing-noreturn"
    while (1) {
        copyUSBufferIfPossible();
        if (cncMemory.state == READY || cncMemory.state == MANUAL_CONTROL)
            tryToStartProgram();
        if (cncMemory.stepperState == DIRECTION_SET && stepTimeHasCome()) {
            if (cncMemory.currentStep.duration) {
                uint16_t steps = 0;
                if (cncMemory.currentStep.axes.xStep)
                    steps |= motorsPinout.xStep;
                if (cncMemory.currentStep.axes.yStep)
                    steps |= motorsPinout.yStep;
                if (cncMemory.currentStep.axes.zStep)
                    steps |= motorsPinout.zStep;
                GPIO_SetBits(motorsPinout.gpio, steps);
                updatePosition(cncMemory.currentStep);
            }
            cncMemory.stepperState = STEP_SET;
            clearStepTimeHasCome();
        }
        if (cncMemory.stepperState == STEP_SET && stepIsOver()) {
            cncMemory.stepperState = NOT_RUNNING;
            STM_EVAL_LEDOff(LED6);
            clearStepIsOver();
        }
        if (cncMemory.state == RUNNING_PROGRAM && cncMemory.stepperState == NOT_RUNNING)
            checkProgramEnd();
        if (cncMemory.stepperState == NOT_RUNNING && !isEmergencyStopped()
                && (cncMemory.state == RUNNING_PROGRAM || cncMemory.state == MANUAL_CONTROL))
            executeNextStep();
    }
#pragma clang diagnostic pop
}

__attribute__ ((used)) void SysTick_Handler(void) {
    cncMemory.tick++;
    periodicUICallback();
}
