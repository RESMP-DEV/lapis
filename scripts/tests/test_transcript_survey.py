import unittest
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import transcript_survey  # noqa: E402


class TranscriptSurveyTest(unittest.TestCase):
    def test_rendered_and_shaped_names_follow_transcript_reader(self):
        self.assertTrue(transcript_survey.rendered_block("document"))
        self.assertFalse(transcript_survey.rendered_block("unknown"))
        for tool in (
            "Monitor",
            "SendMessage",
            "ScheduleWakeup",
            "Skill",
            "TaskStop",
            "ToolSearch",
            "AskUserQuestion",
            "mcp__demo__tool",
        ):
            self.assertTrue(transcript_survey.shaped_tool(tool), tool)
        self.assertFalse(transcript_survey.shaped_tool("UnknownTool"))


if __name__ == "__main__":
    unittest.main()
