/*
 * Copyright 2008, Dominik Geyer
 *
 * This file is part of HoldingNuts.
 *
 * HoldingNuts is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * HoldingNuts is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with HoldingNuts.  If not, see <http://www.gnu.org/licenses/>.
 */


#include <cstdlib>

#include "Tokenizer.hpp"

using namespace std;

bool Tokenizer::isSep(char ch, string sep)
{
	for (unsigned int i=0; i < sep.length(); i++)
	{
		if (sep[i] == ch)
			return true;
	}
	
	return false;
}

bool Tokenizer::parse(string str, string sep)
{
	tokens.clear();
	index = 0;
	
	bool quote_open = false;
	int token_start = -1;
	
	char last_char = '\0';
	
	for (unsigned int i=0; i < str.length(); i++)
	{
		char cur_char = str[i];
		
		if (token_start != -1)
		{
			bool end_tok = false;
			int token_end;
			
			if (!quote_open)
			{
				if (i == str.length()-1 && !isSep(cur_char, sep))
				{
					end_tok = true;
					token_end = i - token_start + 1;
				}
				else if (isSep(cur_char, sep))
				{
					end_tok = true;
					token_end = i - token_start;
				}
			}
			else if (quote_open && (cur_char == '\"' && last_char != '\\'))
			{
				end_tok = true;
				quote_open = false;
				token_end = i - token_start;
			}
			
			if (end_tok)
			{
				tokens.push_back(str.substr(token_start, token_end));
				
				token_start = -1;
			}
		}
		else
		{
			if (!isSep(cur_char, sep))
			{
				if (cur_char == '\"' && last_char != '\\')
				{
					if (i + 1 > str.length() -1)  // sanity check
						token_start = i;
					else
						token_start = i + 1;
					
					quote_open = true;
				}
				else
				{
					if (i == str.length() -1)
						tokens.push_back(str.substr(i, 1)); // end of loop
					else
						token_start = i;
				}
			}
		}
		
		last_char = cur_char;
	}
	
	return true;
}

bool Tokenizer::getNext(string &str)
{
	if (index == tokens.size())
	{
		str = "";
		return false;
	}
	
	str = tokens[index++];
	return true;
}

string Tokenizer::getNext()
{
	if (index == tokens.size())
		return "";
	
	return tokens[index++];
}

string Tokenizer::getTillEnd(char sep)
{
	if (index == tokens.size())
		return "";
	
	string scompl;
	for (unsigned int i=index; i < tokens.size(); i++)
	{
		scompl += tokens[i];
		if (i < tokens.size() - 1)
			scompl += sep;
	}
	
	index = tokens.size();
	
	return scompl;
}

int Tokenizer::getNextInt()
{
	return string2int(getNext());
}

float Tokenizer::getNextFloat()
{
	return string2float(getNext());
}

string Tokenizer::operator[](const unsigned int i) const
{
	if (i < tokens.size())
		return tokens[i];
	else
		return "";
}

int Tokenizer::string2int(string s, unsigned int base)
{
	char *ptr;
	return strtol(s.c_str(), &ptr, base);
}

float Tokenizer::string2float(string s)
{
	return (float)strtod(s.c_str(), NULL);
}
