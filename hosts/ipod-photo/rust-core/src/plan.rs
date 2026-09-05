// Fixed viewport and capability admission for the iPod Photo host.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum PlanError {
    Syntax,
    DuplicateField,
    MissingTarget,
    MissingViewport,
    MissingFeatures,
    TargetMismatch,
    HostAbiMismatch,
    ViewportMismatch,
    UnsupportedFeature,
}

pub struct FixedPlanContract<'a> {
    pub target: &'a str,
    pub host_abi: u32,
    pub width: u32,
    pub height: u32,
    pub presentation: &'a str,
    pub raster_density: u32,
    pub supported_features: &'a [&'a str],
}

struct JsonCursor<'a> {
    bytes: &'a [u8],
    offset: usize,
}

impl<'a> JsonCursor<'a> {
    fn new(bytes: &'a [u8]) -> Self {
        Self { bytes, offset: 0 }
    }

    fn whitespace(&mut self) {
        while matches!(
            self.bytes.get(self.offset),
            Some(b' ' | b'\n' | b'\r' | b'\t')
        ) {
            self.offset += 1;
        }
    }

    fn consume(&mut self, expected: u8) -> bool {
        self.whitespace();
        if self.bytes.get(self.offset) == Some(&expected) {
            self.offset += 1;
            true
        } else {
            false
        }
    }

    fn expect(&mut self, expected: u8) -> Result<(), PlanError> {
        self.consume(expected)
            .then_some(())
            .ok_or(PlanError::Syntax)
    }

    fn comma_before_next(&mut self, closing: u8) -> Result<(), PlanError> {
        self.expect(b',')?;
        self.whitespace();
        if self.bytes.get(self.offset) == Some(&closing) {
            Err(PlanError::Syntax)
        } else {
            Ok(())
        }
    }

    fn string(&mut self) -> Result<&'a [u8], PlanError> {
        self.whitespace();
        if self.bytes.get(self.offset) != Some(&b'"') {
            return Err(PlanError::Syntax);
        }
        self.offset += 1;
        let start = self.offset;
        while let Some(&byte) = self.bytes.get(self.offset) {
            match byte {
                b'"' => {
                    let result = &self.bytes[start..self.offset];
                    self.offset += 1;
                    return Ok(result);
                }
                b'\\' | 0x00..=0x1f => return Err(PlanError::Syntax),
                _ => self.offset += 1,
            }
        }
        Err(PlanError::Syntax)
    }

    fn literal(&mut self, value: &[u8]) -> bool {
        self.whitespace();
        if self.bytes.get(self.offset..self.offset + value.len()) == Some(value) {
            self.offset += value.len();
            true
        } else {
            false
        }
    }

    fn u32(&mut self) -> Result<u32, PlanError> {
        self.whitespace();
        let start = self.offset;
        let mut value = 0u32;
        while let Some(byte @ b'0'..=b'9') = self.bytes.get(self.offset).copied() {
            value = value
                .checked_mul(10)
                .and_then(|v| v.checked_add((byte - b'0') as u32))
                .ok_or(PlanError::Syntax)?;
            self.offset += 1;
        }
        if self.offset == start {
            Err(PlanError::Syntax)
        } else {
            Ok(value)
        }
    }

    fn pair(&mut self) -> Result<(u32, u32), PlanError> {
        self.expect(b'[')?;
        let first = self.u32()?;
        self.expect(b',')?;
        let second = self.u32()?;
        self.expect(b']')?;
        Ok((first, second))
    }

    fn skip_string(&mut self) -> Result<(), PlanError> {
        self.whitespace();
        if self.bytes.get(self.offset) != Some(&b'"') {
            return Err(PlanError::Syntax);
        }
        self.offset += 1;
        while let Some(&byte) = self.bytes.get(self.offset) {
            self.offset += 1;
            match byte {
                b'"' => return Ok(()),
                b'\\' => {
                    let escaped = *self.bytes.get(self.offset).ok_or(PlanError::Syntax)?;
                    self.offset += 1;
                    if escaped == b'u' {
                        for _ in 0..4 {
                            let digit = *self.bytes.get(self.offset).ok_or(PlanError::Syntax)?;
                            if !digit.is_ascii_hexdigit() {
                                return Err(PlanError::Syntax);
                            }
                            self.offset += 1;
                        }
                    } else if !matches!(
                        escaped,
                        b'"' | b'\\' | b'/' | b'b' | b'f' | b'n' | b'r' | b't'
                    ) {
                        return Err(PlanError::Syntax);
                    }
                }
                0x00..=0x1f => return Err(PlanError::Syntax),
                _ => {}
            }
        }
        Err(PlanError::Syntax)
    }

    fn skip_number(&mut self) -> Result<(), PlanError> {
        self.whitespace();
        if self.bytes.get(self.offset) == Some(&b'-') {
            self.offset += 1;
        }
        match self.bytes.get(self.offset).copied() {
            Some(b'0') => self.offset += 1,
            Some(b'1'..=b'9') => {
                self.offset += 1;
                while matches!(self.bytes.get(self.offset), Some(b'0'..=b'9')) {
                    self.offset += 1;
                }
            }
            _ => return Err(PlanError::Syntax),
        }
        if self.bytes.get(self.offset) == Some(&b'.') {
            self.offset += 1;
            let start = self.offset;
            while matches!(self.bytes.get(self.offset), Some(b'0'..=b'9')) {
                self.offset += 1;
            }
            if self.offset == start {
                return Err(PlanError::Syntax);
            }
        }
        if matches!(self.bytes.get(self.offset), Some(b'e' | b'E')) {
            self.offset += 1;
            if matches!(self.bytes.get(self.offset), Some(b'+' | b'-')) {
                self.offset += 1;
            }
            let start = self.offset;
            while matches!(self.bytes.get(self.offset), Some(b'0'..=b'9')) {
                self.offset += 1;
            }
            if self.offset == start {
                return Err(PlanError::Syntax);
            }
        }
        Ok(())
    }

    fn skip_value(&mut self, depth: u32) -> Result<(), PlanError> {
        if depth > 16 {
            return Err(PlanError::Syntax);
        }
        self.whitespace();
        match self.bytes.get(self.offset).copied() {
            Some(b'"') => self.skip_string(),
            Some(b'{') => {
                self.offset += 1;
                self.whitespace();
                if self.consume(b'}') {
                    return Ok(());
                }
                loop {
                    self.skip_string()?;
                    self.expect(b':')?;
                    self.skip_value(depth + 1)?;
                    if self.consume(b'}') {
                        return Ok(());
                    }
                    self.comma_before_next(b'}')?;
                }
            }
            Some(b'[') => {
                self.offset += 1;
                self.whitespace();
                if self.consume(b']') {
                    return Ok(());
                }
                loop {
                    self.skip_value(depth + 1)?;
                    if self.consume(b']') {
                        return Ok(());
                    }
                    self.comma_before_next(b']')?;
                }
            }
            Some(b't') if self.literal(b"true") => Ok(()),
            Some(b'f') if self.literal(b"false") => Ok(()),
            Some(b'n') if self.literal(b"null") => Ok(()),
            Some(b'-' | b'0'..=b'9') => self.skip_number(),
            _ => Err(PlanError::Syntax),
        }
    }
}

fn parse_target(
    cursor: &mut JsonCursor<'_>,
    contract: &FixedPlanContract<'_>,
) -> Result<(), PlanError> {
    cursor.expect(b'{')?;
    let mut seen = 0u32;
    loop {
        if cursor.consume(b'}') {
            break;
        }
        let key = cursor.string()?;
        cursor.expect(b':')?;
        let bit = match key {
            b"hostAbi" => {
                if cursor.u32()? != contract.host_abi {
                    return Err(PlanError::HostAbiMismatch);
                }
                1u32
            }
            b"id" => {
                if cursor.string()? != contract.target.as_bytes() {
                    return Err(PlanError::TargetMismatch);
                }
                2u32
            }
            _ => {
                cursor.skip_value(1)?;
                0u32
            }
        };
        if bit != 0 && seen & bit != 0 {
            return Err(PlanError::DuplicateField);
        }
        seen |= bit;
        if cursor.consume(b'}') {
            break;
        }
        cursor.comma_before_next(b'}')?;
    }
    if seen != 3 {
        return Err(PlanError::MissingTarget);
    }
    Ok(())
}

fn parse_viewport(
    cursor: &mut JsonCursor<'_>,
    contract: &FixedPlanContract<'_>,
) -> Result<(), PlanError> {
    cursor.expect(b'{')?;
    let mut seen = 0u32;
    loop {
        if cursor.consume(b'}') {
            break;
        }
        let key = cursor.string()?;
        cursor.expect(b':')?;
        let bit = match key {
            b"logical" | b"physical" => {
                if cursor.pair()? != (contract.width, contract.height) {
                    return Err(PlanError::ViewportMismatch);
                }
                if key == b"logical" {
                    1u32
                } else {
                    2u32
                }
            }
            b"policy" => {
                if cursor.string()? != b"fixed" {
                    return Err(PlanError::ViewportMismatch);
                }
                4u32
            }
            b"presentation" => {
                if cursor.string()? != contract.presentation.as_bytes() {
                    return Err(PlanError::ViewportMismatch);
                }
                8u32
            }
            b"rasterDensity" => {
                if cursor.u32()? != contract.raster_density {
                    return Err(PlanError::ViewportMismatch);
                }
                16u32
            }
            _ => {
                cursor.skip_value(1)?;
                0u32
            }
        };
        if bit != 0 && seen & bit != 0 {
            return Err(PlanError::DuplicateField);
        }
        seen |= bit;
        if cursor.consume(b'}') {
            break;
        }
        cursor.comma_before_next(b'}')?;
    }
    if seen != 31 {
        return Err(PlanError::MissingViewport);
    }
    Ok(())
}

fn parse_features(
    cursor: &mut JsonCursor<'_>,
    contract: &FixedPlanContract<'_>,
) -> Result<(), PlanError> {
    cursor.expect(b'{')?;
    let mut seen = 0u64;
    loop {
        if cursor.consume(b'}') {
            return Ok(());
        }
        let feature = cursor.string()?;
        cursor.expect(b':')?;
        if !cursor.literal(b"true") {
            return Err(PlanError::UnsupportedFeature);
        }
        let index = contract
            .supported_features
            .iter()
            .position(|item| item.as_bytes() == feature)
            .ok_or(PlanError::UnsupportedFeature)?;
        if index >= 64 {
            return Err(PlanError::UnsupportedFeature);
        }
        let bit = 1u64 << index;
        if seen & bit != 0 {
            return Err(PlanError::DuplicateField);
        }
        seen |= bit;
        if cursor.consume(b'}') {
            return Ok(());
        }
        cursor.comma_before_next(b'}')?;
    }
}

pub fn validate_fixed_plan(plan: &[u8], contract: &FixedPlanContract<'_>) -> Result<(), PlanError> {
    let mut cursor = JsonCursor::new(plan);
    cursor.expect(b'{')?;
    let mut seen = 0u32;
    loop {
        if cursor.consume(b'}') {
            break;
        }
        let key = cursor.string()?;
        cursor.expect(b':')?;
        let bit = match key {
            b"target" => {
                parse_target(&mut cursor, contract)?;
                1u32
            }
            b"viewport" => {
                parse_viewport(&mut cursor, contract)?;
                2u32
            }
            b"features" => {
                parse_features(&mut cursor, contract)?;
                4u32
            }
            _ => {
                cursor.skip_value(1)?;
                0u32
            }
        };
        if bit != 0 && seen & bit != 0 {
            return Err(PlanError::DuplicateField);
        }
        seen |= bit;
        if cursor.consume(b'}') {
            break;
        }
        cursor.comma_before_next(b'}')?;
    }
    cursor.whitespace();
    if cursor.offset != plan.len() {
        return Err(PlanError::Syntax);
    }
    if seen & 1 == 0 {
        return Err(PlanError::MissingTarget);
    }
    if seen & 2 == 0 {
        return Err(PlanError::MissingViewport);
    }
    if seen & 4 == 0 {
        return Err(PlanError::MissingFeatures);
    }
    Ok(())
}
