# ใบงานที่ 7.1  การศึกษากลไก Reset Provisioning 3 รูปแบบ และ NVS Memory Forensics

## 0. กล่าวนำ (Introduction)
เมื่ออุปกรณ์ ESP32 ผ่านการ Provisioning สำเร็จแล้ว ข้อมูล Wi-Fi จะถูกบันทึกไว้ใน NVS Flash Memory อย่างถาวร เมื่อเปิดเครื่องใหม่ เฟิร์มแวร์จะเข้าสู่สถานะ `Already provisioned` และข้ามขั้นตอนการรับข้อมูลใหม่ไปทันที

ในการพัฒนาผลิตภัณฑ์และการทดสอบความปลอดภัย วิศวกรจำเป็นต้องทราบวิธีการล้างค่าคอนฟิก (Factory Reset / Erase Credentials) ซึ่งในใบงานนี้นักศึกษาจะได้ทดลองและเปรียบเทียบกลไกการ Reset ครบทั้ง 3 รูปแบบ:
1. **Developer CLI Reset:** การล้างผ่านคำสั่ง Command Line บนเครื่องคอมพิวเตอร์
2. **Build-time Firmware Reset:** การกำหนดค่าผ่าน `menuconfig`
3. **Consumer Hardware Reset:** การต่อสวิตช์ปุ่มกดภายนอก (**External Pushbutton บน GPIO 18**) เพื่อใช้เป็นปุ่ม Factory Reset ทางกายภาพ เสมือนอุปกรณ์ IoT เชิงพาณิชย์จริง (หลีกเลี่ยงการใช้ปุ่ม BOOT/GPIO 0 ที่เป็น Strapping Pin)

---

## 1. วัตถุประสงค์ (Objectives)
1. ศึกษาและทำความเข้าใจสถานะ `Already provisioned` และการตัดสินใจของ `wifi_prov_mgr_is_provisioned()`
2. สามารถล้างข้อมูลการเชื่อมต่อใน Flash Memory ผ่านคำสั่ง CLI (`idf.py erase-flash` และ `esptool.py`) ได้
3. สามารถกำหนดค่า Build Configuration ใน `menuconfig` เพื่อสั่งรีเซ็ต State Machine ได้
4. เข้าใจข้อจำกัดของ **Strapping Pins (GPIO 0 / Bootloader Trap)** และสามารถต่อสวิตช์ปุ่มกดภายนอก (GPIO 18) เพื่อเขียนโปรแกรม Factory Reset ทางกายภาพได้อย่างถูกต้อง

---

## 2. อุปกรณ์ที่ใช้ในการทดลอง (Equipment)
1. บอร์ดไมโครคอนโทรลเลอร์ ESP32 พร้อมสาย USB
2. สวิตช์ปุ่มกด (Tactile Pushbutton Switch) จำนวน 1 ตัว พร้อมสายต่อ Breadboard
3. ESP-IDF Command Prompt (VS Code Terminal)

> [!IMPORTANT]
> **ทำไมจึงไม่ใช้ปุ่ม BOOT (GPIO 0) กดค้างตอนรีเซ็ตบอร์ด?**
> ขา **GPIO 0** บน ESP32 ทำหน้าที่เป็น **Strapping Pin** สำหรับเลือกโหมดการบูต หากขา GPIO 0 มีสถานะเป็น `LOW (0)` ในจังหวะที่บอร์ดถูกรีเซ็ตหรือจ่ายไฟ ชิป ESP32 จะเข้าสู่โหมด **ROM Download Bootloader** (`waiting for download`) ทันที ทำให้ตัวประมวลผลหยุดรอการแฟลชโปรแกรมและไม่รันโค้ด `app_main()` 
> 
> ดังนั้น ในการออกแบบอุปกรณ์เชิงพาณิชย์ จึงนิยมใช้ขา GPIO ทั่วไป (เช่น **GPIO 18**) ต่อร่วมกับปุ่มกดภายนอกเพื่อทำ Factory Reset แทน

---

## 3. สถาปัตยกรรมและการต่อวงจร (Hardware Wiring & Flow)

### 3.1 การต่อวงจรปุ่มกด Factory Reset (GPIO 18)
- ขาหนึ่งของสวิตช์ปุ่มกด $\rightarrow$ ต่อเข้าขา **GPIO 18** ของ ESP32
- อีกขาหนึ่งของสวิตช์ $\rightarrow$ ต่อลง **GND**
*(เปิดใช้งาน Internal Pull-up Resistor ในโค้ด จึงไม่ต้องต่อตัวต้านทานภายนอกเพิ่ม)*

```mermaid
flowchart TD
    Start["⚡ เริ่มต้นทำงาน (app_main)"] --> Check_GPIO["1. ตรวจสอบปุ่ม Factory Reset (GPIO 18)<br/>ถูกกดค้างไว้ 3 วินาทีหรือไม่?"]
    
    Check_GPIO -- "กดค้างครบ 3 วิ (Low/0)" --> HW_Reset["[Hardware Reset Mode]<br/>สั่ง nvs_flash_erase()<br/>และเข้าสู่ Provisioning"]
    Check_GPIO -- "ไม่ได้กด (High/1)" --> Check_Config["2. ตรวจสอบ Build-time Flag<br/>(#ifdef CONFIG_EXAMPLE_RESET_PROVISIONED)"]
    
    HW_Reset --> Init_Prov["เข้าสู่โหมด Provisioning<br/>(กระจายสัญญาณ BLE / SoftAP)"]
    
    Check_Config -- "เปิดใช้งาน Flag" --> Menu_Reset["[Menuconfig Reset]<br/>เรียก wifi_prov_mgr_reset_provisioning()"]
    Menu_Reset --> Init_Prov
    
    Check_Config -- "ปิดใช้งาน Flag" --> Check_NVS["3. ตรวจสอบค่าใน NVS Flash<br/>wifi_prov_mgr_is_provisioned()"]
    
    Check_NVS -- "true (มีข้อมูลเดิม)" --> STA_Mode["[Already Provisioned]<br/>เริ่ม Wi-Fi Station ทันที"]
    Check_NVS -- "false (ว่างเปล่า/เพิ่งถูกลบด้วย CLI)" --> Init_Prov
```

---

## 4. ขั้นตอนการทดลอง (Step-by-Step Procedures)

### ตอนที่ 1 การล้าง Flash ผ่าน Command Line (Developer Level)
1. เสียบสาย USB เข้ากับคอมพิวเตอร์ ตรวจสอบหมายเลขพอร์ต COM (เช่น `COM24`)
2. เปิด Terminal ในโฟลเดอร์โปรเจกต์ `Week-07-W-iFi-Privisioning/Example_codes/Lab7-1-Reset-and-NVS-Forensics`
3. สั่งล้าง Flash Memory ทั้งหมดของชิปด้วยคำสั่ง:
   ```powershell
   idf.py -p COM24 erase-flash
   ```
4. ทำการ Flash โปรแกรมและเปิด Serial Monitor:
   ```powershell
   idf.py -p COM24 flash monitor
   ```
5. สังเกต Log ว่า ESP32 จะรายงานสถานะ `"Starting provisioning"` และสร้าง QR Code ขึ้นมาบนหน้าจอ

---

### ตอนที่ 2 การบังคับ Reset ผ่าน Menuconfig (Firmware Configuration Level)
1. กดปุ่ม `Ctrl + ]` เพื่อออกจาก Serial Monitor
2. เปิดหน้าต่างคอนฟิกโปรเจกต์:
   ```powershell
   idf.py menuconfig
   ```
3. ใช้ปุ่มลูกศรเลื่อนไปที่หัวข้อ **Example Configuration**
4. เลื่อนไปที่บรรทัด **`Reset Provisioned state (Erase credentials)`** แล้วกดปุ่ม `Spacebar` เพื่อเลือกให้มีเครื่องหมาย `[*]`
5. กดปุ่ม `S` เพื่อบันทึก และ `Q` เพื่อออกจากเมนู
6. สั่ง Build และ Flash โปรแกรม:
   ```powershell
   idf.py -p COM24 flash monitor
   ```
7. สังเกตผลลัพธ์ใน Log: บอร์ดจะทำการล้าง Credentials เก่าทิ้งทุกครั้งที่เปิดเครื่องใหม่

---

### ตอนที่ 3 การสร้างปุ่ม Factory Reset ด้วยฮาร์ดแวร์ภายนอก (GPIO 18)

1. นำสวิตช์ปุ่มกดต่อเข้ากับขา **GPIO 18** และ **GND**
2. เพิ่มฟังก์ชันตรวจสอบปุ่ม Factory Reset ลงในไฟล์ `main/main.c`:

```c
#include "driver/gpio.h"

#define FACTORY_RESET_BUTTON_GPIO  GPIO_NUM_18   // ปุ่ม Factory Reset ภายนอก (ต่อลง GND)

static bool check_factory_reset_button(void)
{
    // กำหนดค่า GPIO 18 เป็น Input พร้อมเปิด Internal Pull-up Resistor
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << FACTORY_RESET_BUTTON_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);

    ESP_LOGI("FACTORY_RESET", "Hold GPIO 18 button for 3 seconds to trigger Factory Reset...");
    
    // ตรวจสอบสถานะปุ่มกดค้าง (Active Low / Logic 0)
    int hold_count = 0;
    while (gpio_get_level(FACTORY_RESET_BUTTON_GPIO) == 0) {
        vTaskDelay(pdMS_TO_TICKS(100));
        hold_count++;
        if (hold_count % 10 == 0) {
            ESP_LOGI("FACTORY_RESET", "Holding button... %d/3 seconds", hold_count / 10);
        }
        if (hold_count >= 30) { // กดค้างครบ 3 วินาที (30 x 100ms)
            ESP_LOGW("FACTORY_RESET", "=================================================");
            ESP_LOGW("FACTORY_RESET", ">>> FACTORY RESET TRIGGERED! ERASING NVS FLASH <<<");
            ESP_LOGW("FACTORY_RESET", "=================================================");
            return true;
        }
    }
    return false;
}
```

3. เรียกใช้งานในตอนเริ่มต้นของฟังก์ชัน `app_main()`:

```c
void app_main(void)
{
    // ตรวจสอบการกดปุ่ม Factory Reset ทางกายภาพ (GPIO 18)
    if (check_factory_reset_button()) {
        ESP_ERROR_CHECK(nvs_flash_erase());
    }

    /* Initialize NVS partition */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }
    // ... โค้ดเดิมต่อจากนี้ ...
```

#### การทดสอบ:
1. ปล่อยให้บอร์ดทำงานปกติ $\rightarrow$ บอร์ดจะจำค่าเดิมได้ (`Already provisioned`)
2. กดปุ่มที่ต่อกับ **GPIO 18 ค้างไว้ 3 วินาที** จากนั้นกดรีเซ็ตบอร์ด หรือกดค้างขณะเปิดเครื่อง
3. สังเกต Serial Monitor: ระบบจะตรวจพบการกดค้าง 3 วินาที และสั่งล้าง NVS Flash เพื่อกลับสู่โหมด Provisioning ทันที!

---

---

## 5. กิจกรรมถอดรหัสซอร์สโค้ดและเขียนผังงาน (Code Deconstruction & Flowchart Assignment)

ให้นักศึกษาศึกษาโค้ดใน `main/main.c` และ `main/led_indicator.c` แล้วเขียน **ผังงาน (Flowchart / State Diagram)** เพื่ออธิบายการตัดสินใจและการทำงานของระบบ:

### ภารกิจที่ 1  ผังงานการตัดสินใจช่วง Bootstrapping & Reset Decision
ให้นักศึกษาวาด Flowchart แสดงลำดับตรรกะการตรวจสอบเงื่อนไขตั้งแต่เริ่มต้นรันฟังก์ชัน `app_main()` โดยต้องครอบคลุม:
1. การตรวจสอบสถานะปุ่ม **GPIO 18** (ตรวจจับการกดค้าง 3 วินาที)
2. การทำงานของ `nvs_flash_init()` และกรณีที่ต้อง `nvs_flash_erase()`
3. การตรวจสอบ Macro `#ifdef CONFIG_EXAMPLE_RESET_PROVISIONED`
4. การเรียกฟังก์ชัน `wifi_prov_mgr_is_provisioned(&provisioned)`
5. จุดแยกสายการทำงานเข้าสู่โหมด **Provisioning Mode** หรือ **Station Mode**


```mermaid
flowchart TD
    Start["⚡ เริ่มต้นทำงาน (app_main)"] --> CheckBtn{"1. ตรวจสอบปุ่ม Factory Reset (GPIO 18)<br/>ถูกกดค้างไว้ 3 วินาทีหรือไม่?"}

    CheckBtn -- "กดค้างครบ 3 วิ (Low/0)" --> HWReset["[Hardware Reset Mode]<br/>สั่ง nvs_flash_erase()<br/>และเข้าสู่ Provisioning"]
    CheckBtn -- "ไม่ได้กด (High/1)" --> CheckFlag{"2. ตรวจสอบ Build-time Flag<br/>(#ifdef CONFIG_EXAMPLE_RESET_PROVISIONED)"}

    CheckFlag -- "เปิดใช้งาน Flag" --> MenuReset["[Menuconfig Reset]<br/>เรียก network_prov_mgr_reset_wifi_provisioning()"]
    CheckFlag -- "ปิดใช้งาน Flag" --> CheckNVS{"3. ตรวจสอบค่าใน NVS Flash<br/>network_prov_mgr_is_wifi_provisioned()"}

    CheckNVS -- "false (ว่างเปล่า/เพิ่งถูกลบด้วย CLI)" --> ProvMode["เข้าสู่โหมด Provisioning<br/>(เริ่ม SoftAP + แสดง QR Code)"]
    CheckNVS -- "true (มีข้อมูลเดิม)" --> StaMode["เข้าสู่โหมด Wi-Fi Station (STA)<br/>(เชื่อมต่อ Wi-Fi ทันที)"]

    HWReset --> ProvMode
    MenuReset --> ProvMode
```


### ภารกิจที่ 2 ผังสถานะการเปลี่ยนจังหวะไฟ LED 1 (Wi-Fi STA Indicator)
ให้นักศึกษาวาด State Diagram แสดงการเปลี่ยนสถานะของ **LED 1 (GPIO 2)**:
- เงื่อนไขใดทำให้ LED 1 เข้าสู่สถานะ `LED_STA_MODE_DISCONNECTED` (กระพริบ 200ms Mark / 200ms Space)
- เงื่อนไขหรือ Event ใดทำให้เปลี่ยนเป็น `LED_STA_MODE_CONNECTED` (Heartbeat 200ms ทุก 1s)

```mermaid
stateDiagram-v2
    [*] --> LED_STA_OFF : เริ่มต้นระบบ (app_main)

    LED_STA_OFF --> LED_STA_DISCONNECTED : WIFI_EVENT_STA_START<br/>(เริ่มต้นเชื่อมต่อ Wi-Fi)

    state "LED_STA_DISCONNECTED<br/>(Alert Mode: ติด 200ms / ดับ 200ms)" as LED_STA_DISCONNECTED
    state "LED_STA_CONNECTED<br/>(Heartbeat Mode: ติด 200ms ทุก 1 วินาที)" as LED_STA_CONNECTED

    LED_STA_DISCONNECTED --> LED_STA_CONNECTED : IP_EVENT_STA_GOT_IP<br/>(เชื่อมต่อสำเร็จและได้รับ IP)
    LED_STA_CONNECTED --> LED_STA_DISCONNECTED : WIFI_EVENT_STA_DISCONNECTED<br/>(สัญญาณหลุด / ถูกตัดการเชื่อมต่อ)
```

---

## 6. ตารางบันทึกผลการทดลอง (Experiment Results)

| รูปแบบ Reset | คำสั่ง/พฤติกรรม | LED หลังเปิดเครื่อง | Serial Monitor |
|---|---|---|---|
| **1. CLI Erase** | `idf.py erase-flash` แล้ว flash ใหม่ | กระพริบ 200/200ms (Disconnected) | `erase-flash` ลบทั้งชิป → หลัง flash ใหม่ขึ้น `NVS is empty` → `Starting provisioning` + QR |
| **2. Menuconfig Flag** | ตั้ง `CONFIG_EXAMPLE_RESET_PROVISIONED=y` แล้ว build/flash | กระพริบ 200/200ms (Disconnected) | ทุกครั้งที่บูตขึ้น `forcing reset` → `Starting provisioning` (ไม่ได้ลบ NVS จริง แค่สั่งข้ามทุกครั้ง) |
| **3. Hardware Button (GPIO18)** | กดปุ่ม GPIO18 ค้าง 3 วิ | กระพริบ 200/200ms (Disconnected) | นับ `1/3→2/3→3/3` → `FACTORY RESET TRIGGERED` → `NVS is empty` → `Starting provisioning` + QR |

---

## 7. คำถามท้ายการทดลอง (Post-Lab Questions)
1. เพราะเหตุใดการกดปุ่ม BOOT (GPIO 0) ค้างไว้ในจังหวะรีเซ็ตบอร์ด จึงทำให้โปรแกรมค้างอยู่ที่ ROM Bootloader และไม่ยอมทำงานต่อ?

- GPIO 0 เป็น Strapping Pin เลือกโหมดบูต ถ้าเป็น LOW ตอนรีเซ็ต ชิปเข้าใจว่าให้เข้า Download Mode รอรับเฟิร์มแวร์ทาง UART จึงไม่ไปรัน `app_main()`



2. เพราะเหตุใดคำสั่ง `idf.py erase-flash` จึงทำให้ข้อมูลเฟิร์มแวร์ Application หายไปด้วย ในขณะที่ `nvs_flash_erase()` ไม่ทำให้เฟิร์มแวร์หาย?

- `erase-flash` ลบทั้งชิปทุก partition (bootloader + app + nvs) เพราะทำงานระดับ physical flash ส่วน `nvs_flash_erase()` เป็น API ที่ลบเฉพาะ partition ชื่อ `nvs` เท่านั้น แอปจึงยังอยู่ครบ


3. การออกแบบปุ่ม Factory Reset บนอุปกรณ์ IoT เชิงพาณิชย์ เหตุใดจึงต้องกำหนดให้ผู้ใช้กดปุ่มค้างไว้ 3-5 วินาที แทนที่จะสั่งลบข้อมูลทันทีที่แตะปุ่มเพียงเสี้ยววินาที?

- เพื่อกันการกดโดนโดยไม่ตั้งใจ (debounce) และยืนยันว่าผู้ใช้ตั้งใจจริง เพราะ Factory Reset ลบข้อมูลแล้วกู้คืนไม่ได้

4. หากอุปกรณ์ IoT ถูกติดตั้งอยู่บนเสาสูงหรือฝังอยู่ในผนัง วิธีการ Reset ทางกายภาพรูปแบบใดเหมาะสมที่สุด?

- ถ้ายังออนไลน์อยู่ ใช้ Remote Reset ผ่าน Cloud/แอปมือถือ สะดวกสุด 
ถ้าต้องไม่พึ่งเน็ตเลย ใช้ Power-Cycle Pattern Detection(ตัด-ต่อไฟซ้ำๆ ในเวลาสั้นให้เฟิร์มแวร์นับแล้วสั่งรีเซ็ตเอง) หรือ Pinhole Reset ต่อสายลงมาที่จุดเข้าถึงง่าย
