import { Component } from "react";

import { Button } from "../Button/Button.jsx";
import { Page, PageHead } from "../Page/Page.jsx";
import { SheetBar } from "../SheetBar/SheetBar.jsx";

// A page that fails to draw says so in its own place, and the window around it
// keeps working - the sidebar, the other pages, the way back. App keys it by
// the page's address, so another page starts afresh.
export class PageGuard extends Component {
	constructor(props) {
		super(props);
		this.state = { hasFailed: false };
	}

	static getDerivedStateFromError() {
		return { hasFailed: true };
	}

	componentDidCatch(error) {
		// Said where a developer looks; the lawyer sees the page below.
		console.error(error);
	}

	render() {
		if (this.state.hasFailed == false) return this.props.children;
		// With the band every page has, so its head never stands under the
		// window's own buttons.
		return (
			<Page bar={<SheetBar />}>
				<PageHead
					title="This page could not be shown"
					line="Nothing was changed. Your other pages work as before: go to your Inbox, or open this page again."
					actions={<Button variant="primary" onClick={() => this.props.onHome?.()}>Go to Inbox</Button>}
				/>
			</Page>
		);
	}
}
