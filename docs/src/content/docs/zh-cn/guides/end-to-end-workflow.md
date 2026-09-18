---
title: 端到端实践指南
description: 从用户请求到 Agent 响应的完整系统流程，以及 Skill 开发实践示例
---

import { Aside } from '@astrojs/starlight/components';
import { LinkCard } from 'starlight-theme-nova/components'

本文档通过具体示例，展示 ESP-Claw 从接收用户消息到 Agent 响应的完整系统流程，并提供 Skill 开发的实践指南。

<LinkCard href="@lang/reference-project/dataflow-and-automation" title="数据流与自动化" description="router_rules.json 完整语法与 Event Router 详细工作流程" />
<LinkCard href="@lang/reference-core/claw-core" title="Agent Core" description="Agent Core 的上下文拼接、工具调用与响应机制" />

## 消息处理全流程

以用户在 IM 中发送"帮我设计一个愤怒小鸟游戏"为例，追踪完整系统流程。

### 第 1 步：IM Capability 接收消息

IM 平台（Web IM、微信、QQ 等）的 Capability 接收到用户消息后，构造 `claw_event_t` 事件并发布到 Event Router：

```
用户发送: "帮我设计一个愤怒小鸟游戏"
    ↓
IM Capability 构建事件:
    {
      source_cap:   "webim",
      event_type:   "message",
      content_type: "text",
      text:         "帮我设计一个愤怒小鸟游戏",
      source_channel: "webim",
      chat_id:      "<user_chat_id>"
    }
    ↓
claw_event_router_publish(&event)
```

### 第 2 步：Event Router 规则匹配

Event Router 任务从队列取出事件，逐条匹配 `router_rules.json` 中的规则：

| 顺序 | 规则 ID | 匹配条件 | 结果 |
|------|---------|---------|------|
| 1 | `im_session_command` | text 以 `/session` 开头 | 不匹配，跳过 |
| 2 | `im_llm_command` | text 以 `/llm` 开头 | 不匹配，跳过 |
| 3 | `im_any_message_working_reply` | 任意 text 消息 | **匹配**（`consume_on_match: false`，继续匹配） |
| 4 | `im_any_message_agent` | 任意 text 消息 | **匹配**（`consume_on_match: true`，停止匹配） |

规则 3 先发送一条即时回复 "🦞 ESP-Claw is snapping on it..."，告知用户请求已被接收。

规则 4 触发 `run_agent` 动作，将请求异步提交给 Agent Core。

### 第 3 步：Agent Core 上下文拼接

`claw_core` 从请求队列取出请求，按顺序调用已注册的 context provider 拼装上下文：

| 顺序 | Provider | 注入内容 |
|------|----------|---------|
| 1 | `claw_memory_profile_provider` | 人设（soul.md）、用户画像（user.md）、身份（identity.md） |
| 2 | `claw_memory_long_term_provider` | 长期记忆记录 |
| 3 | `claw_memory_session_history_provider` | 当前会话的历史消息 |
| 4 | `claw_skill_skills_list_provider` | Skills 目录清单（仅摘要） |
| 5 | `claw_cap_tools_provider` | 当前可见工具列表 |

### 第 4 步：LLM 推理与工具调用

拼装好的 prompt 发送到 LLM 后端。LLM 可能返回工具调用指令，例如：

```json
{
  "tool_calls": [
    {
      "name": "activate_skill",
      "arguments": {"skill_id": "game_dev"}
    }
  ]
}
```

`claw_core` 通过 `claw_cap_call_from_core` 将工具调用分发给对应的 Capability 执行。

工具调用的结果会作为新一轮对话继续发送给 LLM，LLM 再基于工具结果生成最终回复。这个过程可能迭代多轮，直到 LLM 给出纯文本回复。

### 第 5 步：响应回传

Agent 完成推理后，将响应以 `out_message` 事件发布回 Event Router：

```
claw_core 发布事件:
    {
      source_cap:   "claw_core",
      event_type:   "out_message",
      content_type: "text",
      text:         "<Agent 的最终回复>",
      source_channel: "webim",
      chat_id:      "<user_chat_id>"
    }
```

Event Router 匹配到规则 `agent_out_message_send_message`，调用 `send_message` 动作将回复发送到来源 IM 通道。

## 默认路由机制

当没有规则匹配消息事件时，Event Router 提供 `default_route_messages_to_agent` 兜底机制。

在 `edge_agent` 中，该值取决于 LLM 是否已完整配置（API Key、backend_type、model 三项均非空）。若 LLM 已配置，未匹配的消息会自动提交给 Agent 处理。

<Aside type="caution" title="LLM 未配置时 Agent 不可用">
  若 LLM 配置不完整，`claw_core` 不会启动。此时 `run_agent` 动作、默认路由、图片 inspect 等功能完全不可用。
  Event Router、自动化规则、本地 Capability 和 Console REPL 仍可正常使用。
</Aside>

## Skill 开发实践

以创建一个"跑马灯"Skill 为例，展示完整的 Skill 开发流程。

### 方案选择

ESP-Claw 中实现硬件控制有两种主要方式：

| 方式 | 适用场景 | 触发方式 |
|------|---------|---------|
| **Skill + Agent** | 需要 LLM 理解用户意图、灵活控制参数 | 用户通过 IM 对话触发 |
| **Router Rule + Lua 脚本** | 固定逻辑、无需 LLM 参与 | 事件直接触发（如启动、定时、按键） |

跑马灯需要 LLM 理解用户的颜色和速度要求，选择 Skill + Agent 方式。

### 第 1 步：创建 Skill 目录结构

在 DATA 存储区创建 Skill 文件：

```
/fatfs/skills/marquee/
├── SKILL.md
└── scripts/
    └── marquee.lua
```

### 第 2 步：编写 SKILL.md

```markdown
---
{
  "name": "marquee",
  "description": "Run LED marquee running light effect with configurable color and speed. Requires WS2812 LED strip.",
  "metadata": {
    "cap_groups": ["cap_lua"],
    "manage_mode": "readonly"
  },
  "execution": {
    "entry": "scripts/marquee.lua"
  }
}
---

# Marquee Light

Run LED marquee running light effect on WS2812 LED strip.

## Usage

Call `lua_run_script` to execute the marquee script:

```json
{
  "path": "{CUR_SKILL_DIR}/scripts/marquee.lua",
  "args": {
    "color": "#00ff00",
    "speed_ms": 50,
    "length": 30
  }
}
```

### Parameters

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `color` | string | `"#00ff00"` | LED color in hex format |
| `speed_ms` | number | `50` | Delay between steps in milliseconds |
| `length` | number | `30` | Number of LEDs in the strip |

## Stopping

Call `lua_run_script` with `{"action": "stop"}` to stop the marquee.
```

### 第 3 步：编写 Lua 脚本

`scripts/marquee.lua`：

```lua
local led_strip = require("led_strip")
local delay = require("delay")

local args = ... or {}
local color = args.color or "#00ff00"
local speed_ms = args.speed_ms or 50
local length = args.length or 30

-- Initialize LED strip
local strip = led_strip.init({
    gpio = 8,
    num = length,
    model = "ws2812"
})

-- Running light effect
local pos = 0
while true do
    strip:clear()
    strip:set_pixel(pos, color)
    strip:refresh()
    delay.delay_ms(speed_ms)
    pos = (pos + 1) % length
end
```

### 第 4 步：使用

用户在 IM 中发送：

> 帮我跑马灯，绿色，速度快一点

Agent 匹配到 `marquee` Skill，激活后调用 `lua_run_script` 执行脚本。

### 不需要 Agent 的场景

如果跑马灯只需固定效果（如开机自动运行），可以直接用 router rule：

```json
{
  "id": "startup_marquee",
  "match": {
    "source_cap": "app_claw",
    "event_type": "startup",
    "event_key": "boot_completed"
  },
  "actions": [
    {
      "type": "run_script",
      "input": {
        "path": "/fatfs/skills/marquee/scripts/marquee.lua",
        "args": { "color": "#00ff00", "speed_ms": 50 }
      }
    }
  ]
}
```

## router_rules.json 规则设计要点

### 规则顺序决定优先级

规则按数组顺序匹配，先命中的规则优先生效。`consume_on_match` 控制是否继续匹配后续规则：

- `true`：停止匹配，事件已消耗
- `false`：继续匹配后续规则（适合"旁路"逻辑，如发送即时回复）

### 模板变量

规则的动作参数支持模板变量，在运行时替换为实际值：

| 模板 | 来源 | 示例值 |
|------|------|--------|
| `{{event.text}}` | 事件文本 | `"帮我开灯"` |
| `{{event.source_channel}}` | 来源通道 | `"webim"` |
| `{{event.chat_id}}` | 聊天 ID | `"user_123"` |
| `{{match.remainder}}` | 前缀匹配后的剩余文本 | `/session clear` → `"clear"` |
| `{{match.text}}` | 匹配的完整文本 | `"/session clear"` |

### 动作类型对照

| 动作 | 用途 | 是否经过 LLM |
|------|------|-------------|
| `call_cap` | 直接调用 Capability | 否 |
| `run_agent` | 提交给 Agent Core 推理 | 是 |
| `run_script` | 运行 Lua 脚本 | 否 |
| `send_message` | 发送 IM 消息 | 否 |
| `emit_event` | 生成新事件 | 否（但新事件可能触发规则） |
| `drop` | 丢弃事件 | 否 |

### 常见规则模式

**即时回复 + Agent 处理**

先发一条"正在处理"的消息，再提交给 Agent：

```json
[
  {
    "id": "working_reply",
    "consume_on_match": false,
    "match": { "event_type": "message", "content_type": "text" },
    "actions": [{ "type": "send_message", "input": { "message": "Processing..." } }]
  },
  {
    "id": "route_to_agent",
    "consume_on_match": true,
    "match": { "event_type": "message", "content_type": "text" },
    "actions": [{ "type": "run_agent" }]
  }
]
```

**命令前缀匹配**

处理 `/command arg1 arg2` 格式的命令：

```json
{
  "match": {
    "text": "/mycommand",
    "text_match_rule": "prefix"
  },
  "actions": [
    {
      "type": "call_cap",
      "input": { "command": "{{match.remainder}}" }
    }
  ]
}
```

**Agent 输出回传**

将 Agent 响应发送回来源 IM：

```json
{
  "match": {
    "source_cap": "claw_core",
    "event_type": "out_message",
    "content_type": "text"
  },
  "actions": [
    {
      "type": "send_message",
      "input": {
        "channel": "{{event.source_channel}}",
        "chat_id": "{{event.chat_id}}",
        "message": "{{event.text}}"
      }
    }
  ]
}
```

## 常见问题

### Agent 没有回复

检查清单：
1. LLM 是否已配置（API Key + backend_type + model）
2. `router_rules.json` 是否有 `run_agent` 规则或 `default_route_messages_to_agent` 已启用
3. 是否有 `agent_out_message_send_message` 规则将输出回传 IM
4. 出站通道绑定是否正确（`claw_event_router_register_outbound_binding`）

### Skill 未被 Agent 激活

检查清单：
1. Skill 的 `name` 是否与目录名一致
2. `description` 是否包含用户可能使用的关键词
3. Skill 是否出现在 Skills 清单中（通过 `skills_list` 工具查看）
4. `cap_groups` 是否正确声明了需要的工具组

### Lua 脚本执行失败

检查清单：
1. 脚本路径是否正确（使用 `{CUR_SKILL_DIR}/scripts/...`）
2. 所需的 Lua 模块是否已注册
3. 脚本是否有语法错误（通过 Console `lua_run_script` 直接测试）
