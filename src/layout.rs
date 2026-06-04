#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Size {
    pub width: f64,
    pub height: f64,
}

#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Point {
    pub x: f64,
    pub y: f64,
}

#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Margins {
    pub top: f64,
    pub right: f64,
    pub bottom: f64,
    pub left: f64,
}

#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Transformation {
    pub origin_x: f64,
    pub origin_y: f64,
    pub scale_x: f64,
    pub scale_y: f64,
}

#[derive(Clone, Debug, PartialEq)]
pub struct Key {
    pub label: &'static str,
    pub action: &'static str,
    pub width: f64,
    pub height: f64,
    pub class: &'static str,
}

#[derive(Clone, Debug, PartialEq)]
pub struct PlacedKey {
    pub key: Key,
    pub origin: Point,
}

#[derive(Clone, Debug, PartialEq)]
pub struct Row {
    pub keys: Vec<PlacedKey>,
    pub size: Size,
}

#[derive(Clone, Debug, PartialEq)]
pub struct View {
    pub rows: Vec<(Point, Row)>,
    pub size: Size,
}

#[derive(Clone, Debug, PartialEq)]
pub struct Layout {
    pub views: Vec<View>,
    pub margins: Margins,
}

pub const DEFAULT_WIDTH: f64 = 35.33;
pub const DEFAULT_HEIGHT: f64 = 52.0;
pub const ALTLINE_WIDTH: f64 = 52.67;
pub const WIDE_WIDTH: f64 = 62.0;
pub const SPACELINE_WIDTH: f64 = 142.0;
pub const SPECIAL_WIDTH: f64 = 44.0;

impl Row {
    pub fn new(keys: Vec<Key>) -> Self {
        let mut offset = 0.0;
        let mut placed = Vec::with_capacity(keys.len());
        let mut height = 0.0_f64;

        for key in keys {
            height = height.max(key.height);
            placed.push(PlacedKey {
                origin: Point { x: offset, y: 0.0 },
                key: key.clone(),
            });
            offset += key.width;
        }

        Self {
            keys: placed,
            size: Size {
                width: offset,
                height,
            },
        }
    }
}

impl View {
    pub fn new(rows: Vec<Row>) -> Self {
        let width = rows
            .iter()
            .map(|row| row.size.width)
            .fold(0.0_f64, f64::max);
        let mut y = 0.0;
        let mut placed_rows = Vec::with_capacity(rows.len());

        for row in rows {
            let origin = Point {
                x: (width - row.size.width) / 2.0,
                y,
            };
            y += row.size.height;
            placed_rows.push((origin, row));
        }

        Self {
            rows: placed_rows,
            size: Size { width, height: y },
        }
    }

    pub fn super_size<'a>(views: impl Iterator<Item = &'a View>) -> Size {
        views.fold(
            Size {
                width: 0.0,
                height: 0.0,
            },
            |size, view| Size {
                width: size.width.max(view.size.width),
                height: size.height.max(view.size.height),
            },
        )
    }
}

impl Layout {
    pub fn size(&self) -> Size {
        let inner = View::super_size(self.views.iter());
        Size {
            width: self.margins.left + inner.width + self.margins.right,
            height: self.margins.top + inner.height + self.margins.bottom,
        }
    }

    pub fn transformation(&self, available: Size) -> Transformation {
        let size = self.size();
        let h_scale = available.width / size.width;
        let v_scale = available.height / size.height;
        let scale_x = if (h_scale / v_scale) < 1.055 {
            h_scale
        } else {
            v_scale
        };
        let scale_y = h_scale.min(v_scale);

        Transformation {
            origin_x: (available.width - scale_x * size.width) / 2.0
                + scale_x * self.margins.left,
            origin_y: (available.height - scale_y * size.height) / 2.0
                + scale_y * self.margins.top,
            scale_x,
            scale_y,
        }
    }
}

pub fn key(
    label: &'static str,
    action: &'static str,
    width: f64,
    class: &'static str,
) -> Key {
    Key {
        label,
        action,
        width,
        height: DEFAULT_HEIGHT,
        class,
    }
}

pub fn default_key(label: &'static str) -> Key {
    key(label, label, DEFAULT_WIDTH, "")
}

pub fn us_base_view() -> View {
    View::new(vec![
        Row::new("q w e r t y u i o p".split(' ').map(default_key).collect()),
        Row::new("a s d f g h j k l".split(' ').map(default_key).collect()),
        Row::new(vec![
            key("⇧", "Shift", ALTLINE_WIDTH, "action"),
            default_key("z"),
            default_key("x"),
            default_key("c"),
            default_key("v"),
            default_key("b"),
            default_key("n"),
            default_key("m"),
            key("⌫", "Backspace", ALTLINE_WIDTH, "action"),
        ]),
        Row::new(vec![
            key("?123", "Symbols", WIDE_WIDTH, "action"),
            key("拼", "Mode", SPECIAL_WIDTH, "mode"),
            key("space", "Space", SPACELINE_WIDTH, "space"),
            key(".", ".", SPECIAL_WIDTH, ""),
            key("return", "Enter", WIDE_WIDTH, "action"),
        ]),
    ])
}

pub fn symbols_view() -> View {
    View::new(vec![
        Row::new("1 2 3 4 5 6 7 8 9 0".split(' ').map(default_key).collect()),
        Row::new("@ # $ _ & - + ( )".split(' ').map(default_key).collect()),
        Row::new(vec![
            key("123", "Numbers", ALTLINE_WIDTH, "action"),
            default_key("*"),
            default_key("\""),
            default_key("'"),
            default_key(":"),
            default_key(";"),
            default_key("!"),
            default_key("?"),
            key("⌫", "Backspace", ALTLINE_WIDTH, "action"),
        ]),
        Row::new(vec![
            key("ABC", "Main", WIDE_WIDTH, "action"),
            key(",", ",", SPECIAL_WIDTH, ""),
            key("space", "Space", SPACELINE_WIDTH, "space"),
            key(".", ".", SPECIAL_WIDTH, ""),
            key("return", "Enter", WIDE_WIDTH, "action"),
        ]),
    ])
}

pub fn numbers_view() -> View {
    View::new(vec![
        Row::new(vec![key("1", "1", 110.0, ""), key("2", "2", 110.0, ""), key("3", "3", 110.0, "")]),
        Row::new(vec![key("4", "4", 110.0, ""), key("5", "5", 110.0, ""), key("6", "6", 110.0, "")]),
        Row::new(vec![key("7", "7", 110.0, ""), key("8", "8", 110.0, ""), key("9", "9", 110.0, "")]),
        Row::new(vec![
            key("ABC", "Main", WIDE_WIDTH, "action"),
            key("0", "0", 110.0, ""),
            key("⌫", "Backspace", WIDE_WIDTH, "action"),
            key("return", "Enter", 110.0, "action"),
        ]),
    ])
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn row_offsets_accumulate_without_extra_spacing() {
        let row = Row::new(vec![
            default_key("a"),
            key("space", "Space", SPACELINE_WIDTH, "space"),
        ]);

        assert_eq!(row.keys[0].origin.x, 0.0);
        assert_eq!(row.keys[1].origin.x, DEFAULT_WIDTH);
        assert_eq!(row.size.width, DEFAULT_WIDTH + SPACELINE_WIDTH);
    }

    #[test]
    fn rows_are_centered_in_view() {
        let view = us_base_view();
        let first_row_width = DEFAULT_WIDTH * 10.0;
        let second_row_width = DEFAULT_WIDTH * 9.0;

        assert!((view.size.width - first_row_width).abs() < 0.001);
        assert!((view.rows[0].0.x - 0.0).abs() < 0.001);
        assert!((view.rows[1].0.x - (first_row_width - second_row_width) / 2.0).abs() < 0.001);
        assert_eq!(view.size.height, DEFAULT_HEIGHT * 4.0);
    }

    #[test]
    fn transformation_matches_squeekboard_stretch_rule() {
        let layout = Layout {
            views: vec![us_base_view()],
            margins: Margins {
                top: 0.0,
                right: 0.0,
                bottom: 0.0,
                left: 0.0,
            },
        };
        let transform = layout.transformation(Size {
            width: 360.0,
            height: 220.0,
        });

        assert!(transform.scale_x > 0.0);
        assert!(transform.scale_y > 0.0);
        assert!(transform.scale_x >= transform.scale_y);
    }
}
