/**
 * 兼容层：为 Agora libahpl.a 提供 xTaskCreateRestrictedPinnedToCore。
 * 该符号在旧版 ESP-IDF 中存在，新版已移除，此处用 xTaskCreatePinnedToCore 实现。
 */
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

/*
 * 与旧版 FreeRTOS-MPU TaskParameters_t 前若干字段布局兼容的结构，
 * 用于从库传入的指针中读取创建任务所需参数。
 */
typedef struct {
    TaskFunction_t pvTaskCode;
    const char *pcName;
    unsigned short usStackDepth;  /* 栈深度，单位为 word */
    void *pvParameters;
    UBaseType_t uxPriority;
    void *puxStackBuffer;         /* 可选静态栈，NULL 则动态分配 */
    /* 后续可能还有 MemoryRegion_t xRegions[] 等，此处不使用 */
} compat_task_params_t;

BaseType_t xTaskCreateRestrictedPinnedToCore(void *pxTaskDefinition,
                                             TaskHandle_t *pxCreatedTask,
                                             BaseType_t xCoreID)
{
    const compat_task_params_t *p = (const compat_task_params_t *)pxTaskDefinition;
    if (p == NULL) {
        return pdFAIL;
    }
    /* usStackDepth 在 FreeRTOS 中为 words，xTaskCreatePinnedToCore 的 ulStackDepth 为 bytes */
    const uint32_t stack_bytes = (uint32_t)p->usStackDepth * sizeof(StackType_t);
    BaseType_t ret = xTaskCreatePinnedToCore(
        p->pvTaskCode,
        p->pcName,
        stack_bytes,
        p->pvParameters,
        p->uxPriority,
        pxCreatedTask,
        xCoreID
    );
    return ret;
}
