import { createFileRoute } from "@tanstack/react-router";
import { Desk } from "@/components/sentinel/desk";

export const Route = createFileRoute("/")({ component: Desk });
