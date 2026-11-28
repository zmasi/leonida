//! Leonida's D3D12 runtime (the repository's `d3d12/`, built as `leonida.exe`).
//!
//! Its settings are `LEONIDA_*`. Scripts written for the VIctory runtime this code grew from (GPL-3.0, Marvisak and the
//! forks credited in README.md) name them `VICTORY_*`: such a name still works, standing for `LEONIDA_*` where that
//! is not set itself.

/// The `LEONIDA_` settings to set for the environment `vars`: one per `VICTORY_X` whose `LEONIDA_X` is not set.
pub fn settings_names<I>(vars: I) -> Vec<(String, String)>
where
    I: IntoIterator<Item = (String, String)>,
{
    let vars: Vec<(String, String)> = vars.into_iter().collect();
    let set: std::collections::HashSet<&str> = vars.iter().map(|(k, _)| k.as_str()).collect();
    let mut out: Vec<(String, String)> = vars
        .iter()
        .filter_map(|(k, v)| {
            let rest = k.strip_prefix(OLD_PREFIX)?;
            if rest.is_empty() {
                return None;
            }
            let name = format!("LEONIDA_{rest}");
            (!set.contains(name.as_str())).then(|| (name, v.clone()))
        })
        .collect();
    out.sort();
    out
}

/// The prefix the older scripts use (assembled so the rebrand script does not rewrite it).
const OLD_PREFIX: &str = concat!("VIC", "TORY_");

/// Applies [`settings_names`] to this process. Called first thing in `main`, before any thread starts.
pub fn apply_settings_names() {
    for (k, v) in settings_names(std::env::vars()) {
        // SAFETY: called at the start of `main`, before the runtime starts any thread, so nothing reads the
        // environment concurrently.
        unsafe { std::env::set_var(k, v) };
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn vars(list: &[(&str, &str)]) -> Vec<(String, String)> {
        list.iter().map(|(k, v)| ((*k).to_owned(), (*v).to_owned())).collect()
    }

    fn old(rest: &str) -> String {
        format!("{OLD_PREFIX}{rest}")
    }

    #[test]
    fn older_names_stand_for_leonida_ones() {
        let got = settings_names(vec![(old("RENDER_SCALE"), "0.5".into()), ("PATH".into(), "x".into())]);
        assert_eq!(got, vars(&[("LEONIDA_RENDER_SCALE", "0.5")]));
    }

    #[test]
    fn leonida_name_wins() {
        let got = settings_names(vec![(old("RELAY"), "1".into()), ("LEONIDA_RELAY".into(), "2".into())]);
        assert!(got.is_empty());
    }

    #[test]
    fn bare_prefix_is_ignored() {
        assert!(settings_names(vec![(old(""), "1".into())]).is_empty());
    }
}
