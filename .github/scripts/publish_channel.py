#!/usr/bin/env python3
"""
Publish one release asset to the public announcement channel.
"""
import os
import sys
import asyncio
from datetime import datetime, timedelta, timezone

from telethon import TelegramClient
from telethon.extensions import markdown
from telethon.sessions import StringSession
from telethon.tl.functions.messages import SendScheduledMessagesRequest
from telethon.tl.types import DocumentAttributeFilename

CHANNEL = os.environ["TG_PUBLIC_CHANNEL"]
ASSET_PATH = os.environ["ASSET_PATH"]
VERSION = os.environ["ASSET_VERSION"]
BETA = os.environ.get("ASSET_BETA", "0") == "1"
DRY_RUN = os.environ.get("TG_DRY_RUN", "") == "1"
SCHEDULE_DAYS = int(os.environ.get("TG_SCHEDULE_DAYS", "350") or "350")
SEARCH_LIMIT = int(os.environ.get("TG_SEARCH_LIMIT", "200") or "200")
THUMB = os.environ.get("TG_THUMB") or os.path.join(
    os.path.dirname(os.path.abspath(__file__)), "..", "data",
    "logo_256_square.png")

CAPTION = ("— Updated version to "
           "[{label}](https://github.com/telegramdesktop/tdesktop/releases/tag/v{version}).")


def document_name(message):
    document = message.document
    for attribute in document.attributes if document else ():
        if isinstance(attribute, DocumentAttributeFilename):
            return attribute.file_name
    return None


async def find_previous(client, channel, name):
    async for message in client.iter_messages(channel, limit=SEARCH_LIMIT):
        if document_name(message) == name:
            return message
    return None


async def warn_if_unsent(client, channel, name):
    async for message in client.iter_messages(channel, scheduled=True):
        if document_name(message) == name:
            print(f"WARNING: scheduled message #{message.id} carries {name} and "
                  f"has not been sent yet; this upload replies past it.")
            return


async def main():
    if not os.path.isfile(ASSET_PATH):
        sys.exit(f"No asset at {ASSET_PATH!r}.")
    if not os.path.isfile(THUMB):
        sys.exit(f"No thumbnail at {THUMB!r} (forgot to sparse-checkout it?).")

    asset_name = os.path.basename(ASSET_PATH)
    caption = CAPTION.format(
        label=f"{VERSION} beta" if BETA else VERSION,
        version=VERSION)
    size = os.path.getsize(ASSET_PATH) / 1048576
    print(f"File: {ASSET_PATH} ({size:.0f} MiB)")
    print(f"Caption: {caption}")

    shown, entities = markdown.parse(caption)
    print(f"Renders as: {shown}")
    for e in entities:
        print(f"  {type(e).__name__}: {shown[e.offset:e.offset + e.length]!r} "
              f"-> {getattr(e, 'url', '')}")

    if not 0 <= SCHEDULE_DAYS < 365:
        sys.exit(f"TG_SCHEDULE_DAYS must be within 0..364, got {SCHEDULE_DAYS}.")
    when = (datetime.now(timezone.utc) + timedelta(days=SCHEDULE_DAYS)
            if SCHEDULE_DAYS else None)

    api_id = int(os.environ["TG_API_ID"])
    api_hash = os.environ["TG_API_HASH"]
    session = os.environ["TG_SESSION"]

    async with TelegramClient(StringSession(session), api_id, api_hash) as client:
        channel = await client.get_entity(CHANNEL)

        previous = await find_previous(client, channel, asset_name)
        if not previous:
            sys.exit(f"No message with {asset_name} among the last "
                     f"{SEARCH_LIMIT} in {CHANNEL} to reply to.")
        first_line = next(iter((previous.message or "").splitlines()), "")
        print(f"Replying to #{previous.id} of {previous.date:%Y-%m-%d}: "
              f"{first_line!r}")
        if previous.message == shown:
            print(f"Release already posted as message #{previous.id}.")
            return
        await warn_if_unsent(client, channel, asset_name)

        if DRY_RUN:
            print("\n[dry-run] not uploading.")
            return

        if when is None:
            async for scheduled in client.iter_messages(channel, scheduled=True):
                if document_name(scheduled) == asset_name and scheduled.message == shown:
                    await client(SendScheduledMessagesRequest(channel, [scheduled.id]))
                    posted = await find_previous(client, channel, asset_name)
                    if not posted or posted.message != shown:
                        sys.exit("Sent the scheduled message but could not verify the public post.")
                    print(f"Published message #{posted.id} in {CHANNEL}.")
                    return

        msg = await client.send_file(
            channel, ASSET_PATH,
            force_document=True,
            caption=caption,
            parse_mode="md",
            thumb=THUMB,
            reply_to=previous.id,
            schedule=when)
        if when is None:
            print(f"Published message #{msg.id} in {CHANNEL}.")
            return
        print(f"\nscheduled message #{msg.id} in {CHANNEL} for {when:%Y-%m-%d} "
              f"({SCHEDULE_DAYS} days out); send it from the Scheduled queue.")


if __name__ == "__main__":
    asyncio.run(main())
