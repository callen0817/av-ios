# scripts/export_antigravity_history.py
"""
Exports Antigravity SQLite conversation databases into clean, structured Markdown transcripts.
Handles Session 0 (M0-M12 iOS Build & Architecture) and Session 1 (Hardware Connection & Networking).
"""
import sqlite3
import os
import re
import json
import datetime

def parse_pb(data, depth=0):
    i = 0
    res = []
    while i < len(data):
        key = 0
        shift = 0
        while True:
            if i >= len(data):
                return res
            b = data[i]
            i += 1
            key |= (b & 0x7f) << shift
            if not (b & 0x80):
                break
            shift += 7
        fnum = key >> 3
        wire = key & 0x07
        if wire == 0:
            val = 0
            shift = 0
            while True:
                if i >= len(data):
                    return res
                b = data[i]
                i += 1
                val |= (b & 0x7f) << shift
                if not (b & 0x80):
                    break
                shift += 7
            res.append((fnum, 'varint', val))
        elif wire == 2:
            length = 0
            shift = 0
            while True:
                if i >= len(data):
                    return res
                b = data[i]
                i += 1
                length |= (b & 0x7f) << shift
                if not (b & 0x80):
                    break
                shift += 7
            val = data[i:i+length]
            i += length
            sub = None
            if depth < 6:
                try:
                    sub = parse_pb(val, depth + 1)
                except Exception:
                    pass
            res.append((fnum, 'bytes', val, sub))
        elif wire == 1:
            val = data[i:i+8]
            i += 8
            res.append((fnum, '64bit', val))
        elif wire == 5:
            val = data[i:i+4]
            i += 4
            res.append((fnum, '32bit', val))
        else:
            return res
    return res

def extract_step_details(step_type, payload):
    tree = parse_pb(payload)
    if step_type == 14:  # User input
        for item in tree:
            if item[0] == 19 and item[1] == 'bytes' and item[3]:
                for child in item[3]:
                    if child[0] == 2 and child[1] == 'bytes':
                        try:
                            s = child[2].decode('utf-8').strip()
                            if s:
                                return ('USER', s)
                        except Exception:
                            pass
                    if child[0] == 3 and child[1] == 'bytes':
                        try:
                            s = child[2].decode('utf-8').strip()
                            if s:
                                return ('USER', s)
                        except Exception:
                            pass
        # Fallback
        m = re.findall(b'[\x20-\x7E\r\n\t]{15,}', payload)
        for cand in m:
            s = cand.decode('latin-1').strip()
            if not s.startswith('$') and not s.startswith('bot-') and not s.startswith('b$') and not s.startswith('file://') and not s.startswith('10852e94'):
                if ' ' in s and len(s) > 20:
                    return ('USER', s)

    elif step_type == 15:  # Assistant Response
        text = None
        thinking = None
        for item in tree:
            if item[0] == 20 and item[1] == 'bytes' and item[3]:
                for child in item[3]:
                    if child[0] == 1 and child[1] == 'bytes':
                        try:
                            text = child[2].decode('utf-8').strip()
                        except Exception:
                            pass
                    elif child[0] == 3 and child[1] == 'bytes':
                        try:
                            thinking = child[2].decode('utf-8').strip()
                        except Exception:
                            pass
        if text:
            return ('ASSISTANT', text, thinking)

    elif step_type == 132:  # Tool Call
        summary = ''
        cmd = ''
        out = ''
        for item in tree:
            if item[0] == 140 and item[1] == 'bytes' and item[3]:
                for child in item[3]:
                    if child[0] == 1 and child[1] == 'bytes':
                        try:
                            s = child[2].decode('utf-8')
                            if 'toolSummary' in s:
                                summary = s.split('\n')[-1]
                            if 'CommandLine' in s:
                                cmd = s.split('\n')[-1]
                        except Exception:
                            pass
                    elif child[0] == 2 and child[1] == 'bytes' and child[3]:
                        for sc in child[3]:
                            if sc[0] == 1 and sc[1] == 'bytes':
                                try:
                                    out = sc[2].decode('utf-8').strip()
                                except Exception:
                                    pass
        if cmd or summary or out:
            return ('TOOL_CALL', summary or cmd, cmd, out)

    return (None, None)

def export_conversation_to_markdown(db_path, title, conversation_id, output_md_path):
    conn = sqlite3.connect(db_path)
    rows = conn.execute('SELECT idx, step_type, step_payload FROM steps ORDER BY idx').fetchall()

    lines = []
    lines.append(f'# {title}')
    lines.append('')
    lines.append(f'- **Conversation ID**: `{conversation_id}`')
    lines.append(f'- **Source Database**: `{os.path.basename(db_path)}`')
    lines.append(f'- **Total Trajectory Steps**: {len(rows)}')
    lines.append(f'- **Exported Timestamp**: {datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S")}')
    lines.append('')
    lines.append('---')
    lines.append('')

    turn = 1
    for idx, st, payload in rows:
        parsed = extract_step_details(st, payload)
        role = parsed[0]

        if role == 'USER':
            lines.append(f'## Turn {turn} — User Request')
            lines.append('')
            lines.append(parsed[1])
            lines.append('')
            turn += 1

        elif role == 'ASSISTANT':
            lines.append(f'### Assistant Response')
            lines.append('')
            if len(parsed) > 2 and parsed[2]:
                th = parsed[2]
                if len(th) > 1200:
                    th = th[:1200] + '\n... [truncated for readability]'
                lines.append('<details>')
                lines.append('<summary>💭 Internal Thought Process</summary>')
                lines.append('')
                lines.append(th)
                lines.append('')
                lines.append('</details>')
                lines.append('')
            lines.append(parsed[1])
            lines.append('')
            lines.append('---')
            lines.append('')

        elif role == 'TOOL_CALL':
            summary, cmd, out = parsed[1], parsed[2], parsed[3]
            if cmd or out:
                lines.append(f'<details>')
                lines.append(f'<summary>🛠️ Tool Execution: <code>{summary or (cmd[:50] if cmd else "Command")}</code></summary>')
                lines.append('')
                if cmd:
                    lines.append('**Command:**')
                    lines.append('```powershell')
                    lines.append(cmd)
                    lines.append('```')
                    lines.append('')
                if out:
                    lines.append('**Output:**')
                    lines.append('```text')
                    if len(out) > 2500:
                        lines.append(out[:2500] + '\n... [Output truncated for brevity]')
                    else:
                        lines.append(out)
                    lines.append('```')
                    lines.append('')
                lines.append('</details>')
                lines.append('')

    os.makedirs(os.path.dirname(output_md_path), exist_ok=True)
    content_str = '\n'.join(lines)
    content_str = re.sub(r'ghp_[A-Za-z0-9_]{30,}', '[REDACTED_GITHUB_TOKEN]', content_str)
    with open(output_md_path, 'w', encoding='utf-8') as f:
        f.write(content_str)
    print(f'Exported: {output_md_path} ({len(lines)} lines)')

if __name__ == '__main__':
    base_dir = os.path.abspath(os.path.join(os.path.dirname(__file__), '..'))
    out_dir = os.path.join(base_dir, 'docs', 'antigravity-history')

    s0_db = r'C:\Users\calle\.gemini\antigravity\conversations\f30c0f03-90df-453b-9bfe-a8df22bdc9e4.db'
    s1_db = r'C:\Users\calle\.gemini\antigravity\conversations\33b3e0ed-91fd-49bb-811b-ddcc20b427d3.db'

    if os.path.exists(s0_db):
        export_conversation_to_markdown(
            s0_db,
            'Antigravity Session 0: iOS Build, M0–M12 Implementation & FAST-LIVO2 Architecture',
            'f30c0f03-90df-453b-9bfe-a8df22bdc9e4',
            os.path.join(out_dir, '01_SESSION_0_IOS_BUILD_M0_M12.md')
        )

    if os.path.exists(s1_db):
        export_conversation_to_markdown(
            s1_db,
            'Antigravity Session 1: RoboSense Airy Hardware Ethernet Discovery, IP Verification & Live iOS Validation',
            '33b3e0ed-91fd-49bb-811b-ddcc20b427d3',
            os.path.join(out_dir, '02_SESSION_1_AIRY_LIDAR_CONNECTION_TEST.md')
        )
