//! Just enough JSON for `mod.json`: the whole text is read and checked to be
//! well-formed, strings are kept, and the top-level members are found by
//! name. (A JSON crate would put its comparison impls into every crate that
//! shares the build, and the renderer's tests compare plain integers.)

#[derive(Debug, PartialEq)]
pub enum Json {
    Null,
    Bool(bool),
    Number,
    String(String),
    Array,
    Object(Vec<(String, Json)>),
}

const MAX_DEPTH: usize = 32;

pub fn parse(text: &str) -> Result<Json, String> {
    let mut p = Parser {
        bytes: text.as_bytes(),
        at: 0,
    };
    let value = p.value(0)?;
    p.space();
    if p.at != p.bytes.len() {
        return Err(format!("unexpected text at byte {}", p.at));
    }
    Ok(value)
}

struct Parser<'a> {
    bytes: &'a [u8],
    at: usize,
}

impl Parser<'_> {
    fn space(&mut self) {
        while self
            .bytes
            .get(self.at)
            .is_some_and(|b| matches!(b, b' ' | b'\t' | b'\r' | b'\n'))
        {
            self.at += 1;
        }
    }

    fn eat(&mut self, byte: u8) -> Result<(), String> {
        self.space();
        if self.bytes.get(self.at) == Some(&byte) {
            self.at += 1;
            Ok(())
        } else {
            Err(format!("expected `{}` at byte {}", byte as char, self.at))
        }
    }

    fn word(&mut self, word: &str, value: Json) -> Result<Json, String> {
        if self.bytes[self.at..].starts_with(word.as_bytes()) {
            self.at += word.len();
            Ok(value)
        } else {
            Err(format!("unexpected text at byte {}", self.at))
        }
    }

    /// After a member or element: `,` (true) or the closing byte (false).
    fn more(&mut self, close: u8) -> Result<bool, String> {
        self.space();
        match self.bytes.get(self.at) {
            Some(b',') => {
                self.at += 1;
                Ok(true)
            }
            Some(&b) if b == close => {
                self.at += 1;
                Ok(false)
            }
            _ => Err(format!(
                "expected `,` or `{}` at byte {}",
                close as char, self.at
            )),
        }
    }

    fn value(&mut self, depth: usize) -> Result<Json, String> {
        if depth > MAX_DEPTH {
            return Err("nested too deep".into());
        }
        self.space();
        match self.bytes.get(self.at) {
            None => Err("the text ends early".into()),
            Some(b'{') => {
                self.at += 1;
                let mut members = Vec::new();
                self.space();
                if self.bytes.get(self.at) == Some(&b'}') {
                    self.at += 1;
                    return Ok(Json::Object(members));
                }
                loop {
                    self.space();
                    let key = self.string()?;
                    self.eat(b':')?;
                    members.push((key, self.value(depth + 1)?));
                    if !self.more(b'}')? {
                        return Ok(Json::Object(members));
                    }
                }
            }
            Some(b'[') => {
                self.at += 1;
                self.space();
                if self.bytes.get(self.at) == Some(&b']') {
                    self.at += 1;
                    return Ok(Json::Array);
                }
                loop {
                    self.value(depth + 1)?;
                    if !self.more(b']')? {
                        return Ok(Json::Array);
                    }
                }
            }
            Some(b'"') => self.string().map(Json::String),
            Some(b't') => self.word("true", Json::Bool(true)),
            Some(b'f') => self.word("false", Json::Bool(false)),
            Some(b'n') => self.word("null", Json::Null),
            Some(b'-' | b'0'..=b'9') => {
                let start = self.at;
                while self.bytes.get(self.at).is_some_and(|b| {
                    matches!(b, b'+' | b'-' | b'.' | b'e' | b'E') || b.is_ascii_digit()
                }) {
                    self.at += 1;
                }
                let text = std::str::from_utf8(&self.bytes[start..self.at]).unwrap_or("");
                // JSON has no leading zeros or bare signs; f64's parser takes the rest.
                let digits = text.trim_start_matches('-');
                if digits.len() > 1
                    && digits.starts_with('0')
                    && digits.as_bytes()[1].is_ascii_digit()
                {
                    return Err(format!("a bad number at byte {start}"));
                }
                text.parse::<f64>()
                    .map(|_| Json::Number)
                    .map_err(|_| format!("a bad number at byte {start}"))
            }
            Some(_) => Err(format!("unexpected text at byte {}", self.at)),
        }
    }

    fn hex4(&mut self) -> Result<u32, String> {
        let code = self
            .bytes
            .get(self.at..self.at + 4)
            .and_then(|d| std::str::from_utf8(d).ok())
            .filter(|d| d.bytes().all(|b| b.is_ascii_hexdigit()))
            .and_then(|d| u32::from_str_radix(d, 16).ok())
            .ok_or_else(|| format!("a bad unicode escape at byte {}", self.at))?;
        self.at += 4;
        Ok(code)
    }

    fn string(&mut self) -> Result<String, String> {
        if self.bytes.get(self.at) != Some(&b'"') {
            return Err(format!("expected a string at byte {}", self.at));
        }
        self.at += 1;
        let mut out = Vec::new();
        loop {
            let Some(&b) = self.bytes.get(self.at) else {
                return Err("a string is not closed".into());
            };
            self.at += 1;
            match b {
                b'"' => break,
                b'\\' => {
                    let Some(&e) = self.bytes.get(self.at) else {
                        return Err("a string is not closed".into());
                    };
                    self.at += 1;
                    let c = match e {
                        b'"' | b'\\' | b'/' => char::from(e),
                        b'b' => '\u{8}',
                        b'f' => '\u{c}',
                        b'n' => '\n',
                        b'r' => '\r',
                        b't' => '\t',
                        b'u' => {
                            let mut code = self.hex4()?;
                            if (0xD800..0xDC00).contains(&code) {
                                if self.bytes.get(self.at..self.at + 2) != Some(b"\\u") {
                                    return Err("a lone surrogate".into());
                                }
                                self.at += 2;
                                let low = self.hex4()?;
                                if !(0xDC00..0xE000).contains(&low) {
                                    return Err("a lone surrogate".into());
                                }
                                code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
                            }
                            char::from_u32(code).ok_or("a lone surrogate")?
                        }
                        _ => return Err(format!("a bad escape at byte {}", self.at - 1)),
                    };
                    let mut buf = [0u8; 4];
                    out.extend_from_slice(c.encode_utf8(&mut buf).as_bytes());
                }
                0..=0x1F => return Err("a control character in a string".into()),
                b => out.push(b),
            }
        }
        String::from_utf8(out).map_err(|_| "a string is not UTF-8".into())
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn member<'a>(json: &'a Json, key: &str) -> Option<&'a Json> {
        match json {
            Json::Object(members) => members.iter().find(|(k, _)| k == key).map(|(_, v)| v),
            _ => None,
        }
    }

    #[test]
    fn json_is_read_as_json_is() {
        let text = r#"{"a": [1, 2.5e3, -4, {"b": null}], "ok": true, "off": false,
                       "name": "Caf\u00e9 \ud83d\ude00 \"q\" \\ \/ \n", "empty": {}, "list": []}"#;
        let json = parse(text).unwrap();
        assert_eq!(
            member(&json, "name"),
            Some(&Json::String("Caf\u{e9} \u{1f600} \"q\" \\ / \n".into()))
        );
        assert_eq!(member(&json, "ok"), Some(&Json::Bool(true)));
        assert_eq!(member(&json, "off"), Some(&Json::Bool(false)));
        assert_eq!(member(&json, "a"), Some(&Json::Array));
        assert_eq!(member(&json, "empty"), Some(&Json::Object(vec![])));
        assert_eq!(parse("  [ ]  ").unwrap(), Json::Array);
        assert_eq!(parse("\"é\"").unwrap(), Json::String("é".into()));
    }

    #[test]
    fn what_is_not_json_is_refused() {
        for bad in [
            "",
            "{",
            "{\"a\": }",
            "{\"a\" 1}",
            "{\"a\": 1,}",
            "{\"a\": [1,]}",
            "{\"a\": \"\\ud800\"}",
            "{\"a\": \"\\ud800\\u0041\"}",
            "{\"a\": \"\\x\"}",
            "{\"a\": \"\\u12\"}",
            "{\"a\": 01}",
            "{\"a\": -}",
            "{\"a\": 1.2.3}",
            "{} {}",
            "{\"a\": \"x\ny\"}",
            "{\"a\": \"open}",
            "{\"a\": tru}",
            "{a: 1}",
        ] {
            assert!(parse(bad).is_err(), "{bad:?}");
        }
        let deep = format!("{}1{}", "[".repeat(100), "]".repeat(100));
        assert!(parse(&deep).is_err());
        let ok = format!("{}1{}", "[".repeat(20), "]".repeat(20));
        assert!(parse(&ok).is_ok());
    }
}
