/**
 * Guacamole 프로토콜 직렬화 / 역직렬화
 *
 * 형식: LEN.VALUE,LEN.VALUE,...;
 * 예)  7.connect,3.rdp,9.localhost,4.3389,5.admin,8.password;
 */

export interface Instruction {
  opcode: string;
  args: string[];
}

export function serialize(opcode: string, ...args: string[]): string {
  const elements = [opcode, ...args];
  return elements.map(e => `${e.length}.${e}`).join(',') + ';';
}

export function parse(text: string): Instruction[] {
  const instructions: Instruction[] = [];
  let pos = 0;

  while (pos < text.length) {
    const elements: string[] = [];

    while (pos < text.length) {
      const dot = text.indexOf('.', pos);
      if (dot === -1) return instructions;

      const len = parseInt(text.slice(pos, dot), 10);
      if (isNaN(len)) return instructions;

      const valueStart = dot + 1;
      const valueEnd = valueStart + len;
      if (valueEnd > text.length) return instructions;

      elements.push(text.slice(valueStart, valueEnd));
      pos = valueEnd;

      if (pos >= text.length) return instructions;
      const sep = text[pos++];

      if (sep === ';') break;
      if (sep !== ',') return instructions;
    }

    if (elements.length > 0) {
      instructions.push({ opcode: elements[0], args: elements.slice(1) });
    }
  }

  return instructions;
}
