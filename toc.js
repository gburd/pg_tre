// Populate the sidebar
//
// This is a script, and not included directly in the page, to control the total size of the book.
// The TOC contains an entry for each page, so if each page includes a copy of the TOC,
// the total size of the page becomes O(n**2).
class MDBookSidebarScrollbox extends HTMLElement {
    constructor() {
        super();
    }
    connectedCallback() {
        this.innerHTML = '<ol class="chapter"><li class="chapter-item expanded affix "><a href="index.html">Introduction</a></li><li class="chapter-item expanded affix "><li class="part-title">User documentation</li><li class="chapter-item expanded "><a href="pg_tre.html"><strong aria-hidden="true">1.</strong> User Guide</a></li><li class="chapter-item expanded "><a href="migration-from-0.1.0.html"><strong aria-hidden="true">2.</strong> Migration from 0.1.0</a></li><li class="chapter-item expanded affix "><li class="part-title">Architecture</li><li class="chapter-item expanded "><a href="design.html"><strong aria-hidden="true">3.</strong> Design</a></li><li class="chapter-item expanded "><a href="onpage_format.html"><strong aria-hidden="true">4.</strong> On-disk page format</a></li><li class="chapter-item expanded "><a href="perf.html"><strong aria-hidden="true">5.</strong> Performance</a></li><li class="chapter-item expanded affix "><li class="part-title">Design specifications</li><li class="chapter-item expanded "><a href="specs/posting-page-coalescing.html"><strong aria-hidden="true">6.</strong> Posting-page coalescing (v2.0)</a></li><li class="chapter-item expanded "><a href="specs/variable-width-blooms.html"><strong aria-hidden="true">7.</strong> Variable-width per-tuple blooms (v1.3)</a></li><li class="chapter-item expanded affix "><li class="part-title">Project</li><li class="chapter-item expanded "><a href="testing.html"><strong aria-hidden="true">8.</strong> Testing guide</a></li><li class="chapter-item expanded "><a href="release-checklist.html"><strong aria-hidden="true">9.</strong> Release checklist</a></li><li class="chapter-item expanded "><a href="announcement.html"><strong aria-hidden="true">10.</strong> 1.0.0 announcement</a></li></ol>';
        // Set the current, active page, and reveal it if it's hidden
        let current_page = document.location.href.toString().split("#")[0].split("?")[0];
        if (current_page.endsWith("/")) {
            current_page += "index.html";
        }
        var links = Array.prototype.slice.call(this.querySelectorAll("a"));
        var l = links.length;
        for (var i = 0; i < l; ++i) {
            var link = links[i];
            var href = link.getAttribute("href");
            if (href && !href.startsWith("#") && !/^(?:[a-z+]+:)?\/\//.test(href)) {
                link.href = path_to_root + href;
            }
            // The "index" page is supposed to alias the first chapter in the book.
            if (link.href === current_page || (i === 0 && path_to_root === "" && current_page.endsWith("/index.html"))) {
                link.classList.add("active");
                var parent = link.parentElement;
                if (parent && parent.classList.contains("chapter-item")) {
                    parent.classList.add("expanded");
                }
                while (parent) {
                    if (parent.tagName === "LI" && parent.previousElementSibling) {
                        if (parent.previousElementSibling.classList.contains("chapter-item")) {
                            parent.previousElementSibling.classList.add("expanded");
                        }
                    }
                    parent = parent.parentElement;
                }
            }
        }
        // Track and set sidebar scroll position
        this.addEventListener('click', function(e) {
            if (e.target.tagName === 'A') {
                sessionStorage.setItem('sidebar-scroll', this.scrollTop);
            }
        }, { passive: true });
        var sidebarScrollTop = sessionStorage.getItem('sidebar-scroll');
        sessionStorage.removeItem('sidebar-scroll');
        if (sidebarScrollTop) {
            // preserve sidebar scroll position when navigating via links within sidebar
            this.scrollTop = sidebarScrollTop;
        } else {
            // scroll sidebar to current active section when navigating via "next/previous chapter" buttons
            var activeSection = document.querySelector('#sidebar .active');
            if (activeSection) {
                activeSection.scrollIntoView({ block: 'center' });
            }
        }
        // Toggle buttons
        var sidebarAnchorToggles = document.querySelectorAll('#sidebar a.toggle');
        function toggleSection(ev) {
            ev.currentTarget.parentElement.classList.toggle('expanded');
        }
        Array.from(sidebarAnchorToggles).forEach(function (el) {
            el.addEventListener('click', toggleSection);
        });
    }
}
window.customElements.define("mdbook-sidebar-scrollbox", MDBookSidebarScrollbox);
