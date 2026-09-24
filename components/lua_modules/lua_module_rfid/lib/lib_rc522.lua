-- RC522 RFID Reader Lua Driver
-- I2C address: 0x2F (7-bit)
-- Implements ISO14443A card detection with anti-collision

local i2c = require("i2c")

local M = {}

local DEFAULT_ADDR = 0x2F
local DEFAULT_FREQ_HZ = 100000

-- PCD Commands
local CMD_IDLE       = 0x00
local CMD_CALC_CRC   = 0x03
local CMD_TRANSCEIVE = 0x0C
local CMD_SOFT_RESET = 0x0F

-- PICC Commands
local PICC_REQA   = 0x26
local PICC_WUPA   = 0x52
local PICC_SEL_CL1 = 0x93
local PICC_SEL_CL2 = 0x95
local PICC_SEL_CL3 = 0x97
local PICC_HLTA   = 0x50

-- Register addresses
local REG_COMMAND      = 0x01
local REG_COM_INT_REQ  = 0x04
local REG_DIV_INT_REQ  = 0x05
local REG_ERROR        = 0x06
local REG_STATUS2      = 0x08
local REG_FIFO_DATA    = 0x09
local REG_FIFO_LEVEL   = 0x0A
local REG_BIT_FRAMING  = 0x0D
local REG_COLL         = 0x0E
local REG_MODE         = 0x11
local REG_TX_MODE      = 0x12
local REG_RX_MODE      = 0x13
local REG_TX_CONTROL   = 0x14
local REG_TX_ASK       = 0x15
local REG_CRC_MSB      = 0x21
local REG_CRC_LSB      = 0x22
local REG_RF_CFG       = 0x26
local REG_VERSION      = 0x37
local REG_TIMER_MODE   = 0x2A
local REG_TIMER_PRESC  = 0x2B
local REG_TIMER_RELOAD_MSB = 0x2C
local REG_TIMER_RELOAD_LSB = 0x2D

-- Bit masks
local BIT_START_SEND  = 0x80
local BIT_FLUSH_BUFFER = 0x80
local BIT_TX2_RF_EN   = 0x02
local BIT_TX1_RF_EN   = 0x01
local BIT_FORCE_100_ASK = 0x40
local BIT_T_AUTO      = 0x80
local BIT_RX_NO_ERR   = 0x08
local BIT_TX_IRQ      = 0x40
local BIT_RX_IRQ      = 0x20
local BIT_IDLE_IRQ    = 0x10
local BIT_CRC_IRQ     = 0x04
local BIT_TIMER_IRQ   = 0x01

-- Timeouts
local TIMEOUT_MS = 2000
local POLL_INTERVAL_MS = 5

local mt = {}
mt.__index = mt

-- ---------------------------------------------------------------------------
-- Constructor
-- ---------------------------------------------------------------------------

function M.new(opts)
    opts = type(opts) == "table" and opts or {}
    local bus
    local owns_bus = false

    if opts.bus ~= nil then
        bus = opts.bus
    else
        bus = i2c.new(
            assert(opts.port, "rc522.new: missing 'port'"),
            assert(opts.sda, "rc522.new: missing 'sda'"),
            assert(opts.scl, "rc522.new: missing 'scl'"),
            opts.frequency or opts.freq_hz or DEFAULT_FREQ_HZ
        )
        owns_bus = opts.close_bus == true
    end

    local addr = opts.addr or DEFAULT_ADDR
    local dev = bus:device(addr, 0)

    local self = setmetatable({
        _bus = bus,
        _dev = dev,
        _owns_bus = owns_bus,
        _addr = addr,
        _initialized = false,
    }, mt)

    self:_pcd_init()
    return self
end

-- ---------------------------------------------------------------------------
-- Public methods
-- ---------------------------------------------------------------------------

function mt:address()
    return self._addr
end

--- Read firmware version
function mt:read_version()
    return self:_read_reg(REG_VERSION)
end

--- Scan for a card with timeout
-- @param timeout_ms timeout in milliseconds (default 2000)
-- @return table { uid = {bytes}, sak = number, type = string } or nil
function mt:scan(timeout_ms)
    timeout_ms = timeout_ms or TIMEOUT_MS
    local start = self:_millis()

    while (self:_millis() - start) < timeout_ms do
        local result = self:_try_detect_card()
        if result then
            return result
        end
        self:_delay(POLL_INTERVAL_MS)
    end
    return nil
end

--- Halt a card
function mt:halt()
    local data = { PICC_HLTA, 0x00 }
    self:_transceive_short(data, 7)
end

--- Close the device
function mt:close()
    if self._dev then
        pcall(function()
            self:_write_reg(REG_TX_CONTROL, 0x00)  -- disable antenna
        end)
        self._dev:close()
        self._dev = nil
    end
    if self._owns_bus and self._bus then
        self._bus:close()
        self._bus = nil
    end
end

function mt:__gc()
    pcall(function()
        self:close()
    end)
end

-- ---------------------------------------------------------------------------
-- Internal: I2C register access
-- RC522 I2C protocol: write [reg_addr, data...], read [reg_addr | 0x80] then receive
-- ---------------------------------------------------------------------------

function mt:_write_reg(reg, val)
    self._dev:write_byte(val, reg)
end

function mt:_read_reg(reg)
    return self._dev:read_byte(reg)
end

function mt:_read_reg_n(reg, len)
    return self._dev:read(len, reg)
end

function mt:_set_bits(reg, bits)
    local val = self:_read_reg(reg)
    self:_write_reg(reg, val | bits)
end

function mt:_clear_bits(reg, bits)
    local val = self:_read_reg(reg)
    self:_write_reg(reg, val & (~bits))
end

-- ---------------------------------------------------------------------------
-- Internal: PCD initialization
-- ---------------------------------------------------------------------------

function mt:_pcd_init()
    -- Soft reset
    self:_write_reg(REG_COMMAND, CMD_SOFT_RESET)
    self:_delay(50)

    -- Wait for reset to complete (power-down bit clears)
    local start = self:_millis()
    while (self:_millis() - start) < 1000 do
        local cmd = self:_read_reg(REG_COMMAND)
        if (cmd & 0x10) == 0 then  -- power-down bit cleared
            break
        end
        self:_delay(10)
    end

    -- Configure timer
    self:_write_reg(REG_TIMER_MODE, BIT_T_AUTO)
    self:_write_reg(REG_TIMER_PRESC, 0x69)  -- TPrescaler = TModeReg[3:0]:TPrescalerReg = 0x69
    self:_write_reg(REG_TIMER_RELOAD_MSB, 0x03)
    self:_write_reg(REG_TIMER_RELOAD_LSB, 0xE8)  -- ~1000ms timeout

    -- Configure transmitter
    self:_write_reg(REG_TX_ASK, BIT_FORCE_100_ASK)
    self:_write_reg(REG_MODE, 0x3D)  -- CRC preset 0x6363, TxWaitRF

    -- Configure receiver
    self:_write_reg(REG_RX_MODE, BIT_RX_NO_ERR)

    -- Enable antenna
    self:_set_bits(REG_TX_CONTROL, BIT_TX1_RF_EN | BIT_TX2_RF_EN)

    self._initialized = true
end

-- ---------------------------------------------------------------------------
-- Internal: Card detection (ISO14443A anti-collision)
-- ---------------------------------------------------------------------------

function mt:_try_detect_card()
    -- Step 1: REQA - request type A
    local atqa = self:_transceive_bits({ PICC_REQA }, 7)
    if not atqa then
        return nil
    end

    -- Step 2: Anti-collision Level 1
    local uid, sak = self:_anticollision(PICC_SEL_CL1)
    if not uid then
        return nil
    end

    -- Determine card type from SAK
    local card_type = self:_get_card_type(sak, #uid)

    return {
        uid = uid,
        sak = sak,
        type = card_type,
    }
end

function mt:_transceive_bits(data, valid_bits)
    -- Stop any active command
    self:_write_reg(REG_COMMAND, CMD_IDLE)
    self:_clear_bits(REG_COM_INT_REQ, 0x7F)  -- clear all interrupt bits

    -- Flush FIFO
    self:_write_reg(REG_FIFO_LEVEL, BIT_FLUSH_BUFFER)

    -- Set bit framing
    local tx_last_bits = valid_bits % 8
    if tx_last_bits == 0 then tx_last_bits = 0 end
    self:_write_reg(REG_BIT_FRAMING, tx_last_bits)

    -- Write data to FIFO
    for _, b in ipairs(data) do
        self:_write_reg(REG_FIFO_DATA, b)
    end

    -- Start transceive
    self:_write_reg(REG_COMMAND, CMD_TRANSCEIVE)
    self:_set_bits(REG_BIT_FRAMING, BIT_START_SEND)

    -- Wait for completion
    local start = self:_millis()
    while (self:_millis() - start) < 50 do
        local irq = self:_read_reg(REG_COM_INT_REQ)
        if (irq & (BIT_TX_IRQ | BIT_RX_IRQ)) ~= 0 then
            break
        end
        if (irq & BIT_TIMER_IRQ) ~= 0 then
            return nil  -- timeout
        end
        self:_delay(1)
    end

    -- Check for errors
    local err = self:_read_reg(REG_ERROR)
    if (err & 0x1B) ~= 0 then  -- collision, parity, protocol, buffer overflow
        return nil
    end

    -- Read FIFO level
    local fifo_len = self:_read_reg(REG_FIFO_LEVEL)
    if fifo_len == 0 then
        return nil
    end

    -- Read response
    local result = {}
    local raw = self:_read_reg_n(REG_FIFO_DATA, fifo_len)
    for i = 1, #raw do
        result[i] = string.byte(raw, i)
    end

    return result
end

function mt:_anticollision(sel_cmd)
    -- Send anti-collision command: SEL_CMD + NVB (0x20 = 2 complete bytes)
    local data = { sel_cmd, 0x20 }
    local response = self:_transceive_bits(data, 16)
    if not response or #response < 5 then
        return nil
    end

    -- Extract UID (4 bytes) and check BCC
    local uid = {}
    for i = 1, 4 do
        uid[i] = response[i]
    end

    -- Verify BCC (Block Check Code)
    local bcc = response[5]
    local calc_bcc = 0
    for i = 1, 4 do
        calc_bcc = calc_bcc ~ uid[i]
    end
    if calc_bcc ~= bcc then
        return nil
    end

    -- SELECT: send complete UID + BCC
    local select_data = { sel_cmd, 0x70 }  -- NVB = 0x70 (7 complete bytes)
    for i = 1, 4 do
        select_data[i + 2] = uid[i]
    end
    select_data[7] = bcc

    local sak_resp = self:_transceive_bits(select_data, 56)
    if not sak_resp or #sak_resp < 1 then
        return nil
    end

    local sak = sak_resp[1]

    -- Check if UID is complete (cascade bit in SAK)
    if (sak & 0x04) ~= 0 then
        -- Cascade level 2 - multi-part UID
        local uid2 = self:_anticollision_cascade(sel_cmd + 2, uid)
        if uid2 then
            return uid2, sak
        end
        return nil
    end

    return uid, sak
end

function mt:_anticollision_cascade(sel_cmd, partial_uid)
    -- Simplified cascade handling
    local data = { sel_cmd, 0x20 }
    local response = self:_transceive_bits(data, 16)
    if not response or #response < 5 then
        return nil
    end

    local uid_part = {}
    for i = 1, 4 do
        uid_part[i] = response[i]
    end

    -- Remove cascade tag (0x88) if present
    local result = {}
    for _, b in ipairs(partial_uid) do
        if b ~= 0x88 then
            result[#result + 1] = b
        end
    end
    for _, b in ipairs(uid_part) do
        if b ~= 0x88 then
            result[#result + 1] = b
        end
    end

    return result
end

function mt:_get_card_type(sak, uid_len)
    if uid_len == 4 then
        if sak == 0x08 or sak == 0x09 then
            return "MIFARE 1K"
        elseif sak == 0x18 then
            return "MIFARE 4K"
        elseif sak == 0x00 then
            return "MIFARE Ultralight"
        elseif sak == 0x10 or sak == 0x11 then
            return "MIFARE Plus"
        elseif sak == 0x01 then
            return "MIFARE TNP3XXX"
        else
            return string.format("Unknown (SAK=0x%02X)", sak)
        end
    elseif uid_len == 7 then
        if sak == 0x00 then
            return "MIFARE Ultralight C"
        elseif sak == 0x08 or sak == 0x09 then
            return "MIFARE 1K (7-byte UID)"
        else
            return string.format("Unknown 7-byte (SAK=0x%02X)", sak)
        end
    elseif uid_len == 10 then
        return "MIFARE (10-byte UID)"
    end
    return string.format("Unknown (UID len=%d, SAK=0x%02X)", uid_len, sak)
end

-- ---------------------------------------------------------------------------
-- Internal: CRC calculation
-- ---------------------------------------------------------------------------

function mt:_calc_crc(data)
    self:_write_reg(REG_COMMAND, CMD_IDLE)
    self:_clear_bits(REG_DIV_INT_REQ, BIT_CRC_IRQ)
    self:_write_reg(REG_FIFO_LEVEL, BIT_FLUSH_BUFFER)

    for _, b in ipairs(data) do
        self:_write_reg(REG_FIFO_DATA, b)
    end

    self:_write_reg(REG_COMMAND, CMD_CALC_CRC)

    local start = self:_millis()
    while (self:_millis() - start) < 100 do
        local irq = self:_read_reg(REG_DIV_INT_REQ)
        if (irq & BIT_CRC_IRQ) ~= 0 then
            break
        end
        self:_delay(1)
    end

    self:_write_reg(REG_COMMAND, CMD_IDLE)

    local crc_msb = self:_read_reg(REG_CRC_MSB)
    local crc_lsb = self:_read_reg(REG_CRC_LSB)
    return crc_lsb, crc_msb  -- little-endian
end

-- ---------------------------------------------------------------------------
-- Internal: Helpers
-- ---------------------------------------------------------------------------

function mt:_transceive_short(data, valid_bits)
    self:_write_reg(REG_COMMAND, CMD_IDLE)
    self:_clear_bits(REG_COM_INT_REQ, 0x7F)
    self:_write_reg(REG_FIFO_LEVEL, BIT_FLUSH_BUFFER)

    local tx_last_bits = valid_bits % 8
    self:_write_reg(REG_BIT_FRAMING, tx_last_bits)

    for _, b in ipairs(data) do
        self:_write_reg(REG_FIFO_DATA, b)
    end

    self:_write_reg(REG_COMMAND, CMD_TRANSCEIVE)
    self:_set_bits(REG_BIT_FRAMING, BIT_START_SEND)
end

function mt:_millis()
    -- Use a simple counter; actual timing depends on delay implementation
    return os.clock() * 1000
end

function mt:_delay(ms)
    local delay = package.loaded["delay"]
    if delay and delay.delay_ms then
        delay.delay_ms(ms)
    else
        -- Fallback: busy wait (not ideal but works)
        local start = os.clock()
        while (os.clock() - start) * 1000 < ms do
        end
    end
end

return M
